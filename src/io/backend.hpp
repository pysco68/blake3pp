#pragma once

// The portable contract between the engines and the per-OS I/O
// backends. Reads and writes share it: an io_context owns the completion
// mechanism, a file is opened against it, and every transfer is named by
// a caller-owned operation whose callback reports it.
//
// Two contract rules span every operation, so they live here rather than
// on any one requirement below:
//  - Teardown drain: a context's destructor drains every in-flight
//    operation, running no callback. The engines declare their buffer
//    pool member BEFORE it precisely so the drain runs before the pool
//    is freed.
//  - Callback discipline: a transfer's callback runs ONLY inside poll(),
//    on the thread that called poll(). Never from submit_read(),
//    submit_write(), flush(), wake() or a destructor. The pipelines --
//    several windows, then several files, driven as senders from one
//    thread -- depend on there being exactly one place where caller code
//    regains control.
// Internal to src/io/, never installed.

#include <cerrno>
#include <string_view>
#if defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#else
#include <condition_variable>
#include <mutex>
#include <utility>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <system_error>

#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/file_writer.hpp>

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// The O_DIRECT / FILE_FLAG_NO_BUFFERING buffer-and-length granule.
constexpr std::size_t direct_align = 4096;

// The buffer arena of the engines and the driver: one direct-I/O-aligned
// allocation behind an owning pointer, so member declaration order alone
// sequences its release against the backend's drain.
struct aligned_delete {
  void operator()(std::byte* p) const noexcept {
    ::operator delete(p, std::align_val_t{direct_align});
  }
};
using aligned_buffer = std::unique_ptr<std::byte[], aligned_delete>;

[[nodiscard]] inline aligned_buffer make_aligned_buffer(std::size_t bytes) {
  return aligned_buffer(static_cast<std::byte*>(
      ::operator new(bytes, std::align_val_t{direct_align})));
}

// How a file is opened on a context. Reads want a path and maybe direct
// I/O; writes want the file created, and preallocated where the platform
// can, because a write that EXTENDS a file serializes on the inode lock
// and collapses an async direct queue to one stalled write at a time.
// The backends that can preallocate do it here, at the open, not later.
struct file_open_options {
  bool write = false;          // create/truncate for writing
  bool direct_io = false;      // O_DIRECT / FILE_FLAG_NO_BUFFERING
  std::uint64_t preallocate_bytes = 0;  // writes only; 0 for none
};

// What a context needs to know at construction; the engine fills it from
// file_reader_options. A type of its own because the window size and
// queue depth are the engine's, and a context outlives any one file's
// options once several files share it.
struct reader_context_options {
  bool async = true;           // try the OS's async engine at all
  bool offload_submit = true;  // io_uring: issue on io-wq (IOSQE_ASYNC)
};

// The wake primitive behind every context's poll(block). wake() is the
// only member of a context another thread may call, so this is the only
// shared state a context has; everything else is single-threaded by
// contract. A wake issued while nobody is blocked is remembered, and the
// next sleep returns at once -- otherwise a wake racing a poll would be
// lost and the driver would sit in a sleep nothing ends.
//
// Linux gets an eventfd because io_uring can poll it: the uring context
// arms an IORING_OP_POLL_ADD on fd() so one enter() waits for reads and
// wakes alike. Elsewhere a condition variable says the same thing.
#if defined(__unix__) || defined(__APPLE__)
// A file descriptor that closes itself. Moves, does not copy; -1 is
// empty. The borrowed int goes where a system call takes one.
class unique_fd {
 public:
  unique_fd() noexcept = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}
  unique_fd(unique_fd&& o) noexcept : fd_(o.release()) {}
  unique_fd& operator=(unique_fd&& o) noexcept {
    if (this != &o) {
      reset(o.release());
    }
    return *this;
  }
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  ~unique_fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};
#endif  // __unix__ || __APPLE__

class poll_waiter {
 public:
#if defined(__linux__)
  poll_waiter() noexcept : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
  poll_waiter(const poll_waiter&) = delete;
  poll_waiter& operator=(const poll_waiter&) = delete;

  // Pollable by anything that takes a descriptor; -1 if the eventfd
  // could not be created, and sleep() then throws.
  [[nodiscard]] int fd() const noexcept { return fd_.get(); }

  void wake() noexcept {
    const std::uint64_t one = 1;
    // EAGAIN is the counter already saturated, and a wake is pending
    // either way. The result is bound to a variable because a cast to
    // void does not silence warn_unused_result on write().
    const ssize_t n = ::write(fd_.get(), &one, sizeof one);
    (void)n;
  }

  [[nodiscard]] bool take() noexcept {
    std::uint64_t v = 0;
    return ::read(fd_.get(), &v, sizeof v) == static_cast<ssize_t>(sizeof v);
  }

  void sleep() {
    if (!fd_) {
      // poll() ignores a negative descriptor and would wait on nothing,
      // forever. A context whose eventfd could not be created throws
      // instead.
      throw std::system_error(EBADF, std::generic_category(),
                              "blake3pp: no wake descriptor to sleep on");
    }
    ::pollfd p{fd_.get(), POLLIN, 0};
    while (::poll(&p, 1, -1) < 0) {
      if (errno != EINTR) {
        throw_errno("poll(wake)");
      }
    }
  }

 private:
  unique_fd fd_;
#else
  void wake() noexcept {
    {
      const std::lock_guard<std::mutex> lock(m_);
      signalled_ = true;
    }
    cv_.notify_one();
  }

  [[nodiscard]] bool take() noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return std::exchange(signalled_, false);
  }

  void sleep() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [this] { return signalled_; });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool signalled_ = false;
#endif
};

// A read in flight. The caller owns it and keeps it at a fixed address
// from submit_read() until its callback has run; backends derive their
// own op type from this and recover it from whatever the OS hands back
// (io_uring's user_data, IOCP's OVERLAPPED*). `done` is called exactly
// once per submitted read, with an empty error_code on success.
struct read_op_base {
  void (*done)(read_op_base*, std::error_code) noexcept = nullptr;
  void* owner = nullptr;  // for the caller's use
};

// The completion-callback I/O contract.
//
// A context owns the completion mechanism (a ring, a port, a queue) and
// nothing about any particular file. C::file is an open file bound to a
// context: constructed from (C&, const std::filesystem::path&, bool
// direct_io) and exposing size() and name(). A source that has no path
// constructs its file differently -- null_context::file takes a size --
// which is why construction is described here rather than required below.
//
// name() is where the degradation ladder becomes visible: it is settled
// when the file is opened, against the context that opened it, and
// file_reader::backend() reports it verbatim.
//
// Lifetime: every C::file must be destroyed before its context, and the
// context's destructor drains every in-flight read WITHOUT running
// callbacks. The engines' member order is what enforces both.
//
// Errors reach the callback as std::error_code:
//  - errno values in the generic category;
//  - Windows errors in the system category;
//  - a cancel as operation_canceled, an unexpected EOF as EIO.
// poll() throws only when the mechanism itself fails, never for a
// transfer's own error.
//
// A read that in_flight() counts must reach its callback. That is what
// makes a submit failure a degradation rather than an exception: an
// engine that refuses to take a read it was already handed leaves it on
// the deferred list, where poll() reads it synchronously and reports
// whatever happens then. An exception at that point would leave the
// read counted, uncompleted and unreachable, which no caller can
// recover from.
template <class C>
concept io_context = requires(C c, const C cc, typename C::file& f,
                              typename C::read_op& op, std::uint64_t off,
                              std::span<std::byte> buf,
                              std::span<const std::byte> cbuf, bool block,
                              std::uint64_t bytes) {
  typename C::file;
  typename C::read_op;
  requires std::derived_from<typename C::read_op, read_op_base>;
  requires std::default_initializable<typename C::read_op>;
  // The file_reader::backend() string, computed when the file was opened
  // and owned by it: the engine hands it straight to file_reader.
  { f.name() } noexcept -> std::same_as<std::string_view>;
  // Queues one read of exactly buf.size() bytes at off. No syscall, no
  // callback. A window the file cannot take asynchronously (an unaligned
  // length under O_DIRECT, or no ring at all) goes on the context's
  // deferred list and is read synchronously inside poll().
  { c.submit_read(f, off, buf, op) };
  // Queues one write of exactly cbuf.size() bytes at off. Same rules as
  // submit_read: no syscall, no callback, and a write the file cannot
  // take asynchronously -- an unaligned length under O_DIRECT, or no
  // engine at all -- goes on the deferred list and is written inside
  // poll(). The caller's bytes must stay put until the callback runs.
  { c.submit_write(f, off, cbuf, op) };
  // Sets the file's size, for trimming a preallocation back to what was
  // actually written. Only meaningful on a file opened for writing.
  { f.set_size(bytes) };
  // Pushes everything queued to the OS in one call. Never throws: what
  // the OS refuses moves to the deferred list, still owed.
  { c.flush() } noexcept;
  // Reaps completions, reissues short reads, performs at most one
  // deferred synchronous read, and runs the callbacks of every op that
  // finished. Flushes first. With block == true, sleeps until at least
  // one callback ran or wake() was called. Returns callbacks run.
  { c.poll(block) } -> std::same_as<std::size_t>;
  // The only member callable from another thread: makes a blocked poll()
  // return. Idempotent; a wake with no blocked poll is remembered.
  { c.wake() } noexcept;
  { cc.in_flight() } noexcept -> std::same_as<std::size_t>;
  // Waits out every read still owed, running no callback, and forgets
  // the rest: afterwards in_flight() is zero and the context references
  // no read_op, so the caller may destroy them. The context stays
  // usable. The destructor drains too, a no-op after this. Never
  // throws: nothing it could report would change what the caller has
  // to do next.
  { c.drain() } noexcept;
  // Asks the OS to abandon one read. Advisory, and never a completion in
  // itself: the op is still owed its callback and still counted by
  // in_flight(), and the only way it can arrive is a later poll(). What
  // the callback then reports is whichever happened first --
  // operation_canceled if the cancel won, the data if the read did, some
  // other error if the read failed on its own -- so a caller that has to
  // know cannot infer it from having asked.
  //
  // Legal only from the thread that calls poll(), and only for an op
  // that was submitted and whose callback has not run. A backend with no
  // cancel mechanism does nothing, which is a conforming implementation:
  // the read runs to completion and reports normally.
  { c.cancel(op) } noexcept;
};

// The queue of reads a context could not hand to its async engine --
// an unaligned length under O_DIRECT or NO_BUFFERING, or no engine at
// all. Written once: every context that has both paths would otherwise
// carry its own copy of the same four pointers, and the rule that only
// ONE of these runs per poll() is the part that must not drift, since it
// is what bounds how long a caller waits to get control back.
template <class Op>
class deferred_ops {
 public:
  void push(Op& op) noexcept {
    op.next_deferred = nullptr;
    if (tail_ == nullptr) {
      head_ = tail_ = &op;
    } else {
      tail_->next_deferred = &op;
      tail_ = &op;
    }
  }

  // The next one to serve, or null. Removed from the queue by the act of
  // taking it: the caller owes its callback either way.
  [[nodiscard]] Op* take() noexcept {
    Op* const op = head_;
    if (op == nullptr) {
      return nullptr;
    }
    head_ = op->next_deferred;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    return op;
  }

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

  // Forgets every queued op: none was ever issued, so there is nothing
  // to wait for, and none gets its callback.
  void clear() noexcept { head_ = tail_ = nullptr; }

  // Forgets the queued ops `drop` accepts, on the same terms: for a file
  // closing with transfers it could not issue, which name its handles
  // and its share of the count. Returns how many were dropped.
  template <class Pred>
  std::size_t remove_if(Pred drop) noexcept {
    std::size_t dropped = 0;
    Op* prev = nullptr;
    for (Op* op = head_; op != nullptr;) {
      Op* const next = op->next_deferred;
      if (drop(*op)) {
        (prev == nullptr ? head_ : prev->next_deferred) = next;
        if (tail_ == op) {
          tail_ = prev;
        }
        ++dropped;
      } else {
        prev = op;
      }
      op = next;
    }
    return dropped;
  }

 private:
  Op* head_ = nullptr;
  Op* tail_ = nullptr;
};

// Every synchronous source has the same shape under the contract:
// nothing is ever in flight, submit_read() only queues, and poll()
// performs one queued read and runs its callback. Written once here and
// parameterised on the source -- a type that opens a path, knows its
// size and name, and reads a span at an offset -- so pread and stdio
// differ in exactly those four things and share the callback discipline,
// which is the part that must not drift.
template <class Source>
class sync_context {
 public:
  struct read_op : read_op_base {
    std::span<std::byte> buf{};        // reads fill this
    std::span<const std::byte> wbuf{};  // writes read from this
    std::uint64_t off = 0;
    Source* src = nullptr;
    read_op* next_deferred = nullptr;
    bool cancelled = false;
    bool is_write = false;
  };

  class file {
   public:
    file(sync_context&, const std::filesystem::path& path,
         const file_open_options& how)
        : src_(path, how) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return src_.size(); }
    [[nodiscard]] std::string_view name() const noexcept {
      return src_.name();
    }
    void set_size(std::uint64_t bytes) { src_.set_size(bytes); }

   private:
    friend class sync_context;
    Source src_;
  };

  sync_context(const reader_context_options&, unsigned) noexcept {}
  sync_context(const sync_context&) = delete;
  sync_context& operator=(const sync_context&) = delete;
  // Nothing is in flight by construction, so there is nothing to drain
  // and no callback to suppress: the queued ops never complete.

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   read_op& op) {
    op.buf = buf;
    op.wbuf = {};
    op.off = off;
    op.src = &f.src_;
    op.cancelled = false;
    op.is_write = false;
    queue_.push(op);
    queued_++;
  }

  // Queued like a read and performed in poll(): nothing this context does
  // is allowed to happen inside submit.
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, read_op& op) {
    op.buf = {};
    op.wbuf = buf;
    op.off = off;
    op.src = &f.src_;
    op.cancelled = false;
    op.is_write = true;
    queue_.push(op);
    queued_++;
  }

  void flush() noexcept {}

  std::size_t poll(bool block) {
    read_op* const next = queue_.take();
    if (next == nullptr) {
      // Nothing to do: a blocking poll here is the driver waiting for
      // another thread, which is what wake() is for.
      if (block) {
        while (!waiter_.take()) {
          waiter_.sleep();
        }
      }
      return 0;
    }
    read_op& op = *next;
    queued_--;
    std::error_code ec;
    if (op.cancelled) {
      // Nothing was read, so there is no data to prefer over the cancel.
      ec = std::make_error_code(std::errc::operation_canceled);
    } else {
      try {
        if (op.is_write) {
          op.src->write_at(op.off, op.wbuf);
        } else {
          op.src->read_at(op.off, op.buf);
        }
      } catch (const std::system_error& e) {
        ec = e.code();
      }
    }
    op.done(&op, ec);
    return 1;
  }

  void wake() noexcept { waiter_.wake(); }

  // A queued read is dropped by flagging it: the queue is singly linked
  // and poll() reaches every entry anyway, so the flag costs nothing and
  // an unlink would cost a walk.
  void cancel(read_op& op) noexcept { op.cancelled = true; }

  [[nodiscard]] std::size_t in_flight() const noexcept { return queued_; }

  // Nothing was ever issued: the queued reads are forgotten.
  void drain() noexcept {
    queue_.clear();
    queued_ = 0;
  }

 private:
  deferred_ops<read_op> queue_;
  std::size_t queued_ = 0;
  poll_waiter waiter_;
};

}  // namespace blake3pp::detail::io_impl
