#pragma once

// The Darwin backend. macOS has no io_uring; the platform's async story IS
// libdispatch (GCD), and its page-cache bypass is fcntl(F_NOCACHE),
// per-fd rather than per-open, with no alignment contract: unaligned edges
// are silently served through the cache instead of being rejected, so the
// dual-fd tail trick the O_DIRECT and NO_BUFFERING backends need
// disappears here. The backend runs positional pread/pwrite loops on GCD's
// global concurrent pool, straight into the engine's buffer ring;
// dispatch_io is not used because it delivers dispatch_data_t chunks it
// allocated itself, an extra copy the zero-copy pipeline exists to
// avoid. A dispatch_group is the teardown drain and a mutex/condvar
// pair the completion queue. Internal to src/io/, never installed.

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <system_error>
#include <vector>

#include "io/backend.hpp"
#include "io/posix_file.hpp"

namespace blake3pp::detail::io_impl {

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

// The submission half of the machinery: submit() queues a transfer onto
// GCD's global pool inside a dispatch group. The context's done list and
// waiter carry the completion back.
//
// The group exists for teardown. In-flight workers touch the engine's
// buffer pool, so destroy() must wait them out before the pool is freed.
struct gcd_pump {
  dispatch_group_t group = nullptr;

  bool init() noexcept {
    group = dispatch_group_create();
    return group != nullptr;
  }

  // Owning the group means owning its release, also when the owning
  // context's constructor throws after init() and its destructor never
  // runs. destroy() is idempotent, so the context's destructor may call it
  // first.
  ~gcd_pump() { destroy(); }
  gcd_pump() = default;
  gcd_pump(const gcd_pump&) = delete;
  gcd_pump& operator=(const gcd_pump&) = delete;

  void destroy() noexcept {
    if (group != nullptr) {
      dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
      dispatch_release(group);
      group = nullptr;
    }
  }

  // GCD takes a C callback and a cookie, so the cast back to the caller's
  // type lives here, in the one place the boundary forces it.
  template <class T, void (*Run)(T&) noexcept>
  void submit(T& ctx) noexcept {
    dispatch_group_async_f(
        group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &ctx,
        +[](void* p) noexcept { Run(*static_cast<T*>(p)); });
  }
};

// The Darwin reader context: one dispatch group, any number of files,
// reads named by the caller's transfer.
//
// Completions arrive on GCD worker threads, which must NOT run the
// callback: a worker pushes the finished op onto an internal list and
// signals, and poll() drains that list on the thread that called it. The
// signal is the contract's poll_waiter, the same primitive wake() uses,
// so a blocked poll has exactly one thing to wait on whether the news is
// "a read finished" or "another thread wants you back".
class gcd_context {
 public:
  struct transfer : transfer_base {
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    int fd = -1;
    const std::byte* src = nullptr;   // writes read from this
    bool is_write = false;
    int error = 0;              // errno captured by the worker
    posix_file* sync_file = nullptr;  // the deferred path needs the pair
    gcd_context* ctx = nullptr;
    transfer* next_deferred = nullptr;  // queued, not yet read
    transfer* next_done = nullptr;      // finished, not yet reported
  };

  class file {
   public:
    file(gcd_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : write_(how.write) {
      const int base = write_ ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      f_.open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = write_ ? 0 : f_.stat_size();
      // Preallocation belongs to the open for the reason it does
      // everywhere else: an extending write serializes, which collapses
      // an async queue to one write at a time. F_PREALLOCATE is
      // best-effort, and set_size() trims back afterwards.
      if (write_ && how.preallocate_bytes > 0 &&
          preallocate(f_.fd_plain(), how.preallocate_bytes)) {
        prealloc_ = how.preallocate_bytes;
      }
      // Darwin's cache bypass is per-fd, not per-open, and tolerates any
      // alignment, so one fd serves every window, tail included.
      if (how.direct_io && set_nocache(f_.fd_plain())) {
        f_.set_direct();
      }
      name_ = ctx.name_for(f_.direct(), write_);
      ctx_ = &ctx;
    }

    // The descriptor closes with this object, and a dispatch worker may
    // be inside a transfer on it: the context waits those out and forgets
    // what this file left behind first.
    ~file() {
      if (ctx_ != nullptr) {
        ctx_->forget(*this);
      }
    }
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
      prealloc_ = 0;
    }

   private:
    friend class gcd_context;
    posix_file f_;
    gcd_context* ctx_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t prealloc_ = 0;
    bool write_ = false;
    std::string name_ = "pread";
  };

  gcd_context(const reader_context_options& opts, unsigned) {
    if (opts.async && pump_.init()) {
      use_gcd_ = true;
    }
  }
  gcd_context(const gcd_context&) = delete;
  gcd_context& operator=(const gcd_context&) = delete;

  // In-flight workers write into the engine's buffer pool, which is freed
  // just after this: wait them out. A wake nobody consumed dies with the
  // waiter and needs no nudge: nothing here waits on it.
  ~gcd_context() {
    drain();
    pump_.destroy();
  }

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
                   transfer& op) {
    arm(op, f, off, buf.size());
    op.dst = buf.data();
    op.src = nullptr;
    op.is_write = false;
    issue(op);
  }

  // The write half of the same contract. Darwin's F_NOCACHE tolerates any
  // alignment, so unlike O_DIRECT there is no length a write cannot take:
  // the deferred list here is only the no-GCD rung, which is why issue()
  // asks about the engine and nothing else.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) {
    arm(op, f, off, buf.size());
    op.dst = nullptr;
    op.src = buf.data();
    op.is_write = true;
    issue(op);
  }

  // dispatch_group_async_f queues the work itself, so there is nothing
  // batched to push. The contract keeps the call because io_uring has.
  void flush() noexcept {}

  // Darwin has no way to recall a pread a dispatch worker is already
  // inside, and the deferred list is served by the same call on the
  // polling thread. So a cancel changes nothing here and the read runs
  // to completion, which the contract allows: the callback still runs
  // exactly once, from poll(), with whatever the read produced.
  void cancel(transfer&) noexcept {}

  std::size_t poll(bool block) {
    std::size_t ran = drain_done() + run_one_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (waiter_.take()) {
        // Either a wake, or a worker's signal; the list says which.
        ran += drain_done();
        return ran;
      }
      waiter_.sleep();
      ran += drain_done();
      if (ran > 0) {
        return ran;
      }
    }
  }

  void wake() noexcept { waiter_.wake(); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Waits every worker out, then forgets what they finished and what
  // was never started: none of it is reported.
  void drain() noexcept {
    if (use_gcd_) {
      dispatch_group_wait(pump_.group, DISPATCH_TIME_FOREVER);
    }
    done_.clear();
    deferred_.clear();
    in_flight_ = 0;
  }

  // A file about to close takes its transfers with it. The descriptor is
  // the hazard: a worker inside pread() on it would, once the number is
  // recycled by another thread's open(), read a different file's bytes
  // into the engine's buffer and report success. So every worker is
  // waited out before the close, and what this file finished or never
  // started is dropped rather than reported, which is the contract's
  // teardown drain for one file.
  //
  // The group is the context's, so the wait covers sibling files too.
  // That is longer than necessary and harmless: a sibling's finished
  // transfer stays on the done list for a later poll() to report.
  void forget(file& f) noexcept {
    // std::min on both, like the other backends: a counting slip must not
    // underflow a size_t, which is what quiesce() and the stall check read.
    const std::size_t never_started = deferred_.remove_if(
        [&f](const transfer& op) noexcept { return op.sync_file == &f.f_; });
    in_flight_ -= std::min(never_started, in_flight_);
    if (!use_gcd_) {
      return;  // nothing is in a worker: the deferred list is poll()'s
    }
    dispatch_group_wait(pump_.group, DISPATCH_TIME_FOREVER);
    const std::size_t finished = done_.remove_if(
        [&f](const transfer& op) noexcept { return op.sync_file == &f.f_; });
    in_flight_ -= std::min(finished, in_flight_);
  }

 private:
  // What a transfer needs that does not depend on its direction.
  void arm(transfer& op, file& f, std::uint64_t off, std::size_t len) noexcept {
    op.len = len;
    op.off = off;
    op.filled = 0;
    op.error = 0;
    op.fd = f.f_.fd_plain();
    op.sync_file = &f.f_;
    op.ctx = this;
    op.next_done = nullptr;
    in_flight_++;
  }

  // The only rung: a worker if there is a dispatch group, otherwise the
  // deferred list that poll() serves on the calling thread.
  void issue(transfer& op) noexcept {
    if (use_gcd_) {
      pump_.submit<transfer, &gcd_context::run_transfer>(op);
    } else {
      deferred_.push(op);
    }
  }

  // Runs on a GCD worker: one positional read or write loop over the
  // caller's buffer, then the op goes on the done list. noexcept: the
  // error travels in the op to whoever calls poll().
  static void run_transfer(transfer& op) noexcept {
    gcd_context& c = *op.ctx;
    int err = 0;
    std::size_t got = 0;
    while (got < op.len) {
      const ssize_t n =
          op.is_write
              ? ::pwrite(op.fd, op.src + got, op.len - got,
                         static_cast<off_t>(op.off + got))
              : ::pread(op.fd, op.dst + got, op.len - got,
                        static_cast<off_t>(op.off + got));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        err = errno;
        break;
      }
      if (n == 0) {
        // A read that ends early ended the file; a write that moves no
        // bytes would spin, so both stop here.
        err = EIO;
        break;
      }
      got += static_cast<std::size_t>(n);
    }
    op.filled = got;
    op.error = err;
    c.done_.push(op);
    c.waiter_.wake();
  }

  // Takes the whole finished list and runs their callbacks here, on the
  // caller's thread. The list is LIFO, which the contract permits: the
  // engine matches a completion to its window by the op, not by order.
  std::size_t drain_done() noexcept {
    transfer* head = done_.take_all();
    std::size_t ran = 0;
    while (head != nullptr) {
      transfer* const next = head->next_done;
      in_flight_--;
      head->done(head, head->error != 0
                           ? std::error_code(head->error,
                                             std::generic_category())
                           : std::error_code{});
      ran++;
      head = next;
    }
    return ran;
  }

  std::size_t run_one_deferred() noexcept {
    transfer* const next = deferred_.take();
    if (next == nullptr) {
      return 0;
    }
    transfer& op = *next;
    std::error_code ec;
    try {
      if (op.is_write) {
        op.sync_file->pwrite_all(op.fd, op.src, op.len, op.off);
      } else {
        op.sync_file->pread_all(op.fd, op.dst, op.len, op.off);
      }
      op.filled = op.len;
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    in_flight_--;
    op.done(&op, ec);
    return 1;
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

  // Finished transfers, pushed by GCD workers and drained by poll(). The
  // link is the op's own next_done, so nothing allocates, and the head is
  // reachable only through these members, each under the lock.
  class done_list {
   public:
    void push(transfer& op) noexcept {
      const std::lock_guard<std::mutex> lk(m_);
      op.next_done = head_;
      head_ = &op;
    }

    // The whole list, LIFO, which the contract permits: the engine matches
    // a completion to its window by the op, not by order.
    [[nodiscard]] transfer* take_all() noexcept {
      const std::lock_guard<std::mutex> lk(m_);
      return std::exchange(head_, nullptr);
    }

    // Forgets everything on the list: nothing here gets its callback.
    void clear() noexcept {
      const std::lock_guard<std::mutex> lk(m_);
      head_ = nullptr;
    }

    // Unlinks and forgets the ops `drop` accepts, for a file closing with
    // transfers that finished but were never reported. Returns how many.
    template <class Pred>
    std::size_t remove_if(Pred drop) noexcept {
      const std::lock_guard<std::mutex> lk(m_);
      std::size_t dropped = 0;
      transfer** link = &head_;
      while (*link != nullptr) {
        transfer* const op = *link;
        if (drop(*op)) {
          *link = op->next_done;
          op->next_done = nullptr;
          ++dropped;
        } else {
          link = &op->next_done;
        }
      }
      return dropped;
    }

   private:
    std::mutex m_;
    transfer* head_ = nullptr;
  };

  gcd_pump pump_;
  poll_waiter waiter_;
  deferred_ops<transfer> deferred_;
  done_list done_;
  std::size_t in_flight_ = 0;
  bool use_gcd_ = false;
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(io_context<gcd_context>);

}  // namespace blake3pp::detail::io_impl

#endif  // __APPLE__
