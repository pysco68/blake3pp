#pragma once

// The Darwin backend. macOS has no io_uring; the platform's async story IS
// libdispatch (GCD), and its page-cache bypass is fcntl(F_NOCACHE),
// per-fd rather than per-open, with no alignment contract: unaligned edges
// are silently served through the cache instead of being rejected, so the
// dual-fd tail trick the O_DIRECT and NO_BUFFERING backends need
// disappears here. The backend runs positional pread/pwrite loops on GCD's
// global concurrent pool, straight into the engine's buffers;
// dispatch_io is not used because it delivers dispatch_data_t chunks it
// allocated itself, an extra copy. A dispatch_group is the teardown
// drain, and a finished transfer lands on the deferred pump's done list,
// which signals the contract's poll_waiter as a lane does. Without a
// group, transfers go to the pump's lanes. Internal to src/io/, never
// installed.

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

#include "io/backend.hpp"
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

// The Darwin reader context: one dispatch group, any number of files,
// reads named by the caller's transfer.
//
// Completions arrive on GCD worker threads and on the deferred pump's
// lanes, neither of which runs the callback: both put the finished op on
// the pump's done list and signal, and poll() reports the list on the
// thread that called it. The signal is the contract's poll_waiter, the
// same primitive wake() uses, so a blocked poll has one thing to wait on.
class gcd_context {
 public:
  class file;

  struct transfer : performed_transfer {
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    int fd = -1;
    const std::byte* src = nullptr;   // writes read from this
    bool is_write = false;
    // Whose transfer this is: forget() matches on it, the file counts it,
    // and the deferred path reads through its descriptors.
    file* owner_file = nullptr;
    gcd_context* ctx = nullptr;
    // The pump's one link: queued for a lane, or finished and not yet
    // reported, never both.
    transfer* next_deferred = nullptr;
    // A cancel was asked for; one is enough. Only the deferred pump reads
    // it, and only for a transfer no lane has taken yet: a dispatch
    // worker already inside pread cannot be recalled on Darwin, which is
    // what cancel() below says.
    bool cancelled = false;
  };

  class file {
   public:
    file(gcd_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : ctx_(&ctx), write_(how.write) {
      const int base = write_ ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      f_.open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = write_ ? 0 : f_.stat_size();
      // Preallocation belongs to the open for the reason it does
      // everywhere else: an extending write serializes, which collapses
      // an async queue to one write at a time. F_PREALLOCATE is
      // best-effort, and set_size() trims back afterwards.
      if (write_ && how.preallocate_bytes > 0) {
        [[maybe_unused]] const bool reserved =
            preallocate(f_.fd_plain(), how.preallocate_bytes);
      }
      // Darwin's cache bypass is per-fd, not per-open, and tolerates any
      // alignment, so one fd serves every window, tail included.
      if (how.direct_io && set_nocache(f_.fd_plain())) {
        f_.set_direct();
      }
      name_ = ctx.name_for(f_.direct(), write_);
    }

    // The descriptor closes with this object, and a dispatch worker or a
    // lane may be inside a transfer on it: the context waits those out and
    // forgets what this file left behind first. The operations must still
    // be alive (the contract's operation lifetime rule).
    ~file() { ctx_->forget(*this); }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is.
    void set_size(std::uint64_t bytes) {
      if (::ftruncate(f_.fd_plain(), static_cast<off_t>(bytes)) != 0) {
        throw_errno("ftruncate");
      }
    }

   private:
    friend class gcd_context;
    posix_file f_;
    gcd_context* ctx_;
    // This file's share of the context's count, so a file that owes
    // nothing closes without waiting on its siblings' transfers.
    std::size_t inflight_ = 0;
    std::uint64_t size_ = 0;
    bool write_ = false;
    std::string name_ = "pread";
  };

  gcd_context(const reader_context_options& opts, unsigned max_inflight)
      : group_(opts.async ? dispatch_group_create() : nullptr),
        queue_depth_(std::max(1u, max_inflight)),
        use_gcd_(static_cast<bool>(group_)),
        // Declared last, and initialized last: see the member list.
        pump_(io_lanes::waker<gcd_context>(), this, &perform_deferred,
              io_lanes_for(max_inflight)) {}
  gcd_context(const gcd_context&) = delete;
  gcd_context& operator=(const gcd_context&) = delete;

  // In-flight workers write into the engine's buffer pool, which is freed
  // just after this: wait them out. A wake nobody consumed dies with the
  // waiter and needs no nudge: nothing here waits on it.
  ~gcd_context() { drain(); }

  // This context's rung and the one it degrades to; see
  // uring_context::engaged_rung().
  static constexpr io_engine rung_engine = io_engine::gcd;
  using rung_below = posix_sync_rung;

  // libdispatch is part of the platform, so the only way a dispatch
  // group fails to be created is that the allocation failed -- which is
  // a fact about this moment and never settled.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return use_gcd_ ? rung_verdict{rung_engine, true}
                    : rung_verdict{rung_below::rung_engine, false};
  }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    arm(op, f, off, buf.size());
    op.dst = buf.data();
    op.src = nullptr;
    op.is_write = false;
    issue(op);
  }

  // The write half of the same contract. Darwin's F_NOCACHE tolerates any
  // alignment, so there is no length a write cannot take: the pump here
  // serves only a context without a dispatch group, and issue() asks
  // about nothing else.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) noexcept {
    arm(op, f, off, buf.size());
    op.dst = nullptr;
    op.src = buf.data();
    op.is_write = true;
    issue(op);
  }

  // dispatch_group_async_f queues the work itself, so there is nothing
  // batched to push. The contract keeps the call because io_uring has one.
  void flush() noexcept {}

  // Darwin has no way to recall a pread a dispatch worker is already
  // inside. A transfer no lane has taken yet is flagged and reports
  // operation_canceled; anything else runs to completion and reports
  // once, from poll(), with whatever the transfer produced.
  void cancel(transfer& op) noexcept { pump_.cancel(op); }

  std::size_t poll(bool block) {
    std::size_t ran = serve_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (waiter_.take()) {
        // A wake, a dispatch worker's signal or a lane's; the done list
        // says which.
        return ran + serve_deferred();
      }
      waiter_.sleep();
      ran += serve_deferred();
      if (ran > 0) {
        return ran;
      }
    }
  }

  void wake() noexcept { waiter_.wake(); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // GCD's global queue takes as many as the caller asked for; without a
  // dispatch group the deferred pump's lanes are the whole of what the
  // device sees.
  [[nodiscard]] unsigned device_depth() const noexcept {
    return use_gcd_ ? queue_depth_ : pump_.depth();
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }

#if defined(BLAKE3PP_TEST_IO_SEAMS)
  // Holds the dispatch group the way a worker still inside a transfer
  // does, until released: a sibling in flight, as long as the test likes.
  void test_hold_group() noexcept { dispatch_group_enter(group_.get()); }
  void test_release_group() noexcept { dispatch_group_leave(group_.get()); }
  [[nodiscard]] bool test_uses_gcd() const noexcept { return use_gcd_; }
#endif

  // Waits every dispatch worker and lane out, then forgets what they
  // finished and what was never started: none of it is reported.
  void drain() noexcept {
    if (use_gcd_) {
      group_.wait();
    }
    pump_.clear();
    in_flight_ = 0;
  }

  // A file about to close takes its transfers with it. The descriptor is
  // the hazard: a worker inside pread() on it would, once the number is
  // recycled by another thread's open(), read a different file's bytes
  // into the engine's buffer and report success. So every dispatch worker
  // and lane is waited out before the close, and what this file finished
  // or never started is dropped unreported: the contract's teardown drain
  // for one file. Legal only while the operations are alive.
  //
  // The group and the lanes are the context's, so the wait covers sibling
  // files too, and a sibling's finished transfer stays on the done list
  // for a later poll() to report.
  void forget(file& f) noexcept {
    // A file that owes nothing has nothing in a worker or on a lane. A
    // count a context-wide drain left stale only brings back the wait.
    if (f.inflight_ == 0) {
      return;
    }
    // std::min keeps a counting slip from underflowing the counts.
    const auto mine = [&f](const transfer& op) noexcept {
      return op.owner_file == &f;
    };
    const auto dropped = [&](std::size_t n) noexcept {
      in_flight_ -= std::min(n, in_flight_);
      f.inflight_ -= std::min(n, f.inflight_);
    };
    std::size_t never_started = pump_.remove_queued_if(mine);
    pump_.quiesce();
    if (use_gcd_) {
      group_.wait();  // a dispatch worker lands what it holds first
    }
    never_started += pump_.remove_done_if(mine);
    dropped(never_started);
  }

 private:
  // What a transfer needs that does not depend on its direction.
  void arm(transfer& op, file& f, std::uint64_t off, std::size_t len) noexcept {
    op.len = len;
    op.off = off;
    op.cancelled = false;
    op.fd = f.f_.fd_plain();
    op.owner_file = &f;
    op.ctx = this;
    op.next_deferred = nullptr;
    op.reset_performance();
    in_flight_++;
    f.inflight_++;
  }

  // A dispatch worker if there is a group, otherwise a lane of the
  // deferred pump.
  void issue(transfer& op) noexcept {
    if (use_gcd_) {
      group_.async<transfer, &gcd_context::run_transfer>(op);
    } else {
      pump_.push(op);
    }
  }

  // Runs on a GCD worker: the same transfer a lane performs, charged to
  // the device around itself, then the op lands on the pump's done list,
  // which wakes poll(). `op.ctx` is read before the op is handed over,
  // and nothing touches the op after: once it has landed, poll() may
  // report it and the caller reuse it.
  static void run_transfer(transfer& op) noexcept {
    op.device_begin();
    op.set_result(perform_deferred(op));
    op.device_end();
    op.ctx->pump_.land(op);
  }

  // One transfer, on a lane or a dispatch worker. Touches the operation
  // and the file it names, and nothing else of this context.
  static std::error_code perform_deferred(transfer& op) noexcept {
    try {
      if (op.is_write) {
        op.owner_file->f_.pwrite_all(op.fd, {op.src, op.len}, op.off);
      } else {
        op.owner_file->f_.pread_all(op.fd, {op.dst, op.len}, op.off);
      }
    } catch (const std::system_error& e) {
      return e.code();
    } catch (...) {
      // posix_file throws only system_error; anything else is reported as
      // EIO so it cannot terminate a noexcept lane.
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  // Runs the callbacks of the transfers the dispatch workers and the
  // lanes finished, on this thread. The inline step serves a machine that
  // would not give the pump a thread.
  std::size_t serve_deferred() noexcept {
    return pump_.report_finished([this](transfer& op) noexcept {
      device_.add_if_served(op.device_total_ns());
      in_flight_--;
      op.owner_file->inflight_--;
      op.done(&op, op.result());
    });
  }

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

  // Waited out by drain(), which the destructor runs before any member
  // goes, because a dispatch worker lands on the pump and wakes the
  // waiter. Declared first, so use_gcd_ is initialized from it.
  unique_dispatch_group group_;
  poll_waiter waiter_;
  std::size_t in_flight_ = 0;
  // What the device itself took, stamped by whichever worker carried the
  // transfer -- a dispatch one or a lane.
  device_time device_{};
  // What the caller asked to have in flight at once; device_depth()
  // reports it where the dispatch queue took the transfers.
  unsigned queue_depth_ = 1;
  bool use_gcd_ = false;
  // The done list every finished transfer lands on, and the lanes that
  // serve transfers when there is no dispatch group. Declared last, so the
  // destructor's join runs while the waiter its wake() reaches is still
  // alive. Do not move this up.
  deferred_pump<transfer> pump_;
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(io_context<gcd_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // __APPLE__
