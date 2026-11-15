#pragma once

// The Darwin backend. macOS has no io_uring; the platform's async story IS
// libdispatch (GCD), and its page-cache bypass is fcntl(F_NOCACHE),
// per-fd rather than per-open, with no alignment contract: unaligned edges
// are silently served through the cache instead of being rejected, so the
// dual-fd tail trick the O_DIRECT and NO_BUFFERING backends need
// disappears here. The backend runs positional pread/pwrite loops on GCD's
// global concurrent pool, straight into the engine's buffers;
// dispatch_io is not used because it delivers dispatch_data_t chunks it
// allocated itself, an extra copy. The pool is the engine under the shared
// completion_context; a dispatch_group is what teardown waits on. Without
// a group, transfers go to the context's lanes. Internal to src/io/, never
// installed.

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#include "io/fd.hpp"
#include "io/io_context.hpp"
#include "io/lanes.hpp"
#include "io/options.hpp"
#include "io/completion_context.hpp"
#include "io/posix_file.hpp"

namespace blake3pp::detail::io_impl {

#if defined(BLAKE3PP_TEST_IO_SEAMS)
// The test executable compiles this backend with a seam, under a name of
// its own (see uring_backend.hpp).
inline namespace test_seams {
#endif

// Turns the page cache off for this fd: Darwin's O_DIRECT analogue.
inline bool set_nocache(int fd) noexcept {
  return ::fcntl(fd, F_NOCACHE, 1) != -1;
}

// fallocate's Darwin twin: reserve the extents (contiguous if the volume
// can, scattered otherwise), then give the file its final logical size,
// so queued writes land as overwrites instead of size-extending appends.
inline bool preallocate(int fd, std::uint64_t len) noexcept {
  fstore_t st{};
  st.fst_flags = F_ALLOCATECONTIG;
  st.fst_posmode = F_PEOFPOSMODE;
  st.fst_offset = 0;
  st.fst_length = static_cast<off_t>(len);
  if (::fcntl(fd, F_PREALLOCATE, &st) == -1) {
    st.fst_flags = F_ALLOCATEALL;
    if (::fcntl(fd, F_PREALLOCATE, &st) == -1) {
      return false;
    }
  }
  return ::ftruncate(fd, static_cast<off_t>(len)) == 0;
}

// A dispatch group, owned: async() queues work onto GCD's global pool
// inside it, wait() waits that work out, and the destructor waits once
// more and releases it. The group exists for teardown: in-flight workers
// touch the engine's buffer pool, which must not be freed under them.
// Null when it was not created, which async() and wait() must not meet.
class unique_dispatch_group {
 public:
  explicit unique_dispatch_group(dispatch_group_t g) noexcept : g_(g) {}
  ~unique_dispatch_group() {
    if (g_ != nullptr) {
      wait();
      dispatch_release(g_);
    }
  }
  unique_dispatch_group(const unique_dispatch_group&) = delete;
  unique_dispatch_group& operator=(const unique_dispatch_group&) = delete;

  explicit operator bool() const noexcept { return g_ != nullptr; }
  [[nodiscard]] dispatch_group_t get() const noexcept { return g_; }

  void wait() noexcept { dispatch_group_wait(g_, DISPATCH_TIME_FOREVER); }

  // GCD takes a C callback and a cookie, so the cast back to the caller's
  // type lives here, in the one place the boundary forces it.
  template <class T, void (*Run)(T&) noexcept>
  void async(T& arg) noexcept {
    dispatch_group_async_f(
        g_, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &arg,
        +[](void* p) noexcept { Run(*static_cast<T*>(p)); });
  }

 private:
  dispatch_group_t g_;
};

// The Darwin engine for completion_context: one dispatch group, any
// number of files, reads named by the caller's transfer.
//
// To the context the dispatch pool is a kernel queue that never refuses a
// non-empty transfer, cannot recall one it has started, and completes
// each in one piece. A worker carries the rest of the transfer, stamps
// the device around its own pread, and lands the transfer on this
// engine's list; next() reports what landed, on the thread that polls.
// The list and the wake share one lock and one condition, so a blocked
// poll has one thing to wait on. Without a dispatch group every transfer
// goes to the context's lanes, and next() waits only for a wake.
class gcd_engine {
 public:
  struct transfer_part : posix_transfer {
    gcd_engine* engine = nullptr;  // where the worker lands it
    transfer_part* next_landed = nullptr;
  };

  class file_part : public posix_file {
   public:
    file_part(const gcd_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      const int base = how.write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = how.write ? 0 : stat_size();
      // Preallocation belongs to the open for the reason it does
      // everywhere else: an extending write serializes, which collapses
      // an async queue to one write at a time. F_PREALLOCATE is
      // best-effort, and set_size() trims back afterwards.
      if (how.write && how.preallocate_bytes > 0) {
        [[maybe_unused]] const bool reserved =
            preallocate(fd_plain(), how.preallocate_bytes);
      }
      // Darwin's cache bypass is per-fd, not per-open, and tolerates any
      // alignment, so one fd serves every window, tail included.
      if (how.direct_io && set_nocache(fd_plain())) {
        set_direct();
      }
      name_ = e.name_for(direct(), how.write);
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is.
    void set_size(std::uint64_t bytes) {
      if (::ftruncate(fd_plain(), static_cast<off_t>(bytes)) != 0) {
        throw_errno("ftruncate");
      }
    }

   private:
    std::uint64_t size_ = 0;
    std::string name_;
  };

  gcd_engine(const reader_context_options& opts, unsigned max_inflight)
      : queue_depth_(std::max(1u, max_inflight)),
        group_(opts.async ? dispatch_group_create() : nullptr),
        use_gcd_(static_cast<bool>(group_)) {}
  gcd_engine(const gcd_engine&) = delete;
  gcd_engine& operator=(const gcd_engine&) = delete;

  // This engine's rung and the one it degrades to; see
  // uring_engine::engaged_rung().
  static constexpr io_engine rung_engine = io_engine::gcd;
  using rung_below = posix_sync_rung;
  // What failed, as the shared context's messages name it.
  [[nodiscard]] std::string_view mechanism() const noexcept {
    return "libdispatch";
  }

  // libdispatch is part of the platform, so the only way a dispatch
  // group fails to be created is that the allocation failed -- which is
  // a fact about this moment and never settled.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return use_gcd_ ? rung_verdict{rung_engine, true}
                    : rung_verdict{rung_below::rung_engine, false};
  }
  [[nodiscard]] bool engaged() const noexcept { return use_gcd_; }
  // GCD's global queue takes as many as the caller asked for; without a
  // dispatch group the lanes are the whole of what the device sees.
  [[nodiscard]] unsigned device_depth(bool, unsigned lanes) const noexcept {
    return use_gcd_ ? queue_depth_ : lanes;
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return perform_rest(op);
  }

  // The rest of the transfer goes to a dispatch worker. Declined without
  // a dispatch group, and for an empty transfer, whose completion would
  // read as a piece that moved nothing: the lanes complete those.
  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    if (!use_gcd_ || op.remaining() == 0) {
      return false;
    }
    transfer_part& part = op;
    part.engine = this;
    ++owed_;
    group_.async<transfer_part, &gcd_engine::run_transfer>(part);
    return true;
  }

  // Darwin has no way to recall a pread a dispatch worker is already
  // inside, and every transfer placed here is in a worker's hands, so it
  // runs to completion and reports what it produced. The worker does not
  // read op.cancelled, which the polling thread writes.
  template <class Op>
  void cancel(Op&) noexcept {}

  // dispatch_group_async_f queues the work itself: nothing is batched,
  // nothing is refused, and nothing waits to be taken back.
  template <class Op, class Refused>
  void flush(Refused&&) noexcept {}
  template <class Op, class Refused>
  void take_back(Refused&&) noexcept {}
  [[nodiscard]] std::error_code take_failure() noexcept { return {}; }

  // One landed transfer, or a wake, or nothing. Everything that landed
  // is taken under one lock and handed out from here one at a time.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    if (taken_.empty()) {
      std::unique_lock<std::mutex> lock(m_);
      for (;;) {
        if (!landed_.empty()) {
          taken_ = std::exchange(landed_, {});
          break;
        }
        if (std::exchange(woken_, false)) {
          return {event::kind::wake};
        }
        if (!block) {
          return {};
        }
        cv_.wait(lock);
      }
    }
    Op& op = static_cast<Op&>(*taken_.take());
    --owed_;
    if (const std::error_code ec = op.result()) {
      return {event::kind::completion, &op, ec, 0};
    }
    return {event::kind::completion, &op, {}, op.remaining()};
  }

  void wake() noexcept {
    {
      const std::lock_guard<std::mutex> lock(m_);
      woken_ = true;
    }
    cv_.notify_one();
  }

  // Nothing a worker holds can be recalled, so a closing file waits for
  // its transfers to land.
  void cancel_file(file_part&, std::size_t) noexcept {}
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&&) noexcept {
    return 0;
  }
  // Transfers given to a worker whose landing next() has not taken.
  [[nodiscard]] std::size_t kernel_owed() const noexcept { return owed_; }
  void begin_drain() noexcept {}
  // A worker lands its transfer and then signals, so a drain that has
  // taken every landing may still have a worker inside the signal,
  // touching this engine. The group's wait outlasts the worker's function
  // itself.
  void end_drain() noexcept {
    if (use_gcd_) {
      group_.wait();
    }
  }
  template <class Op>
  void release(Op&) noexcept {}

  // The degradation strings are observable API, so the synchronous rung
  // keeps the spellings the other backends use.
  [[nodiscard]] std::string name_for(bool direct, bool write) const {
    const char* const sync_name = write ? "pwrite" : "pread";
    const char* const fast_name = write ? "gcd-write" : "gcd";
    std::string n = use_gcd_ ? fast_name : sync_name;
    if (direct) {
      n += "+nocache";
    }
    return n;
  }

#if defined(BLAKE3PP_TEST_IO_SEAMS)
  // Holds the dispatch group the way a worker still inside a transfer
  // does, until released: a sibling in flight, as long as the test likes.
  void test_hold_group() noexcept { dispatch_group_enter(group_.get()); }
  void test_release_group() noexcept { dispatch_group_leave(group_.get()); }
  [[nodiscard]] bool test_uses_gcd() const noexcept { return use_gcd_; }
#endif

 private:
  // Runs on a GCD worker: the rest of the transfer, charged to the device
  // around itself, then the landing. op.filled is left to the context,
  // which advances it by what the completion reports. The engine is read
  // before the transfer lands, and nothing touches the transfer after:
  // once it has landed, the polling thread may report it and the caller
  // reuse or destroy it and its file.
  static void run_transfer(transfer_part& op) noexcept {
    gcd_engine* const e = op.engine;
    op.device_begin();
    op.set_result(carry_rest(op));
    op.device_end();
    {
      const std::lock_guard<std::mutex> lock(e->m_);
      e->landed_.push(op);
    }
    e->cv_.notify_one();
  }

  using landing_list = deferred_ops<transfer_part, &transfer_part::next_landed>;

  std::mutex m_;
  std::condition_variable cv_;
  landing_list landed_;   // under m_: what workers finished
  bool woken_ = false;    // under m_: a wake next() has not reported
  landing_list taken_;    // the polling thread's: landed, not yet handed out
  std::size_t owed_ = 0;  // the polling thread's
  // What the caller asked to have in flight at once; device_depth()
  // reports it where the dispatch queue took the transfers.
  unsigned queue_depth_ = 1;
  // Declared after everything a worker touches, so it is destroyed first
  // and its wait outlasts every worker.
  unique_dispatch_group group_;
  bool use_gcd_ = false;
};

using gcd_context = completion_context<gcd_engine>;

// Definition-site conformance check (see uring_backend.hpp).
static_assert(io_context<gcd_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // __APPLE__
