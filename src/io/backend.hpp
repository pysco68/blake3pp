#pragma once

// The portable contract between the file_reader/file_writer engines and
// the per-OS I/O backends. 
//
// The two sides have different shapes. A WRITER backend is per file and
// owns its slots: start_write() names a buffer by slot and wait_slot()
// blocks for it. A READER runs on reader_context, where the completion
// mechanism is separate from the file, a read is named by a caller-owned
// operation, and completions arrive as callbacks.
//
// Three contract rules span every operation, so they live here rather
// than on any one requirement below:
//  - Slot exclusivity (writers): start_write() may only be called for a
//    slot the backend claimed via wants_async(...), and only while
//    nothing else is outstanding on that slot.
//  - Teardown drain: a backend's or context's destructor drains every
//    in-flight operation, running no callback. The engines declare their
//    buffer pool member BEFORE it precisely so the drain runs before the
//    pool is freed.
//  - Callback discipline (readers): a read's callback runs ONLY inside
//    poll(), on the thread that called poll(). Never from submit_read(),
//    flush(), wake() or a destructor. Everything a later phase wants to
//    build on this -- several windows, then several files, driven as
//    senders from one thread -- depends on there being exactly one place
//    where caller code regains control.
// Internal to src/io/, never installed.

#include <cerrno>
#include <string_view>
#if defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#else
#include <condition_variable>
#include <mutex>
#include <utility>
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

// What a context needs to know at construction; the engine fills it from
// file_reader_options. Deliberately not file_reader_options itself: the
// window size and queue depth are the engine's business, and a context
// outlives any one file's options once several files share it.
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
class poll_waiter {
 public:
#if defined(__linux__)
  poll_waiter() noexcept : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
  poll_waiter(const poll_waiter&) = delete;
  poll_waiter& operator=(const poll_waiter&) = delete;
  ~poll_waiter() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  // Pollable by anything that takes a descriptor; -1 if the eventfd
  // could not be created, which leaves sleep() an error rather than a
  // silent hang.
  [[nodiscard]] int fd() const noexcept { return fd_; }

  void wake() noexcept {
    const std::uint64_t one = 1;
    // EAGAIN is the counter already saturated: a wake is pending either
    // way, which is exactly what the caller asked for. The result is
    // bound rather than cast away, which is what silences the
    // warn_unused_result on write().
    const ssize_t n = ::write(fd_, &one, sizeof one);
    (void)n;
  }

  [[nodiscard]] bool take() noexcept {
    std::uint64_t v = 0;
    return ::read(fd_, &v, sizeof v) == static_cast<ssize_t>(sizeof v);
  }

  void sleep() {
    if (fd_ < 0) {
      // poll() ignores a negative descriptor and would then wait on
      // nothing, forever, which is the one outcome a caller cannot
      // recover from or even see. A context whose eventfd could not be
      // created says so instead.
      throw std::system_error(EBADF, std::generic_category(),
                              "blake3pp: no wake descriptor to sleep on");
    }
    ::pollfd p{fd_, POLLIN, 0};
    while (::poll(&p, 1, -1) < 0) {
      if (errno != EINTR) {
        throw_errno("poll(wake)");
      }
    }
  }

 private:
  int fd_ = -1;
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

// The completion-callback reader contract.
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
// Errors reach the callback as std::error_code: -res in the generic
// category, and an unexpected EOF as EIO. poll() throws only when the
// mechanism itself fails while waiting, never for a read's own error.
//
// A read that in_flight() counts must reach its callback. That is what
// makes a submit failure a degradation rather than an exception: an
// engine that refuses to take a read it was already handed leaves it on
// the deferred list, where poll() reads it synchronously and reports
// whatever happens then. An exception at that point would leave the
// read counted, uncompleted and unreachable, which no caller can
// recover from.
template <class C>
concept reader_context = requires(C c, const C cc, typename C::file& f,
                                  typename C::read_op& op, std::uint64_t off,
                                  std::span<std::byte> buf, bool block) {
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
  // deferred list and is read synchronously inside poll(), which is where
  // the old contract's "lazy at delivery" ended up.
  { c.submit_read(f, off, buf, op) };
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
    std::span<std::byte> buf{};
    std::uint64_t off = 0;
    Source* src = nullptr;
    read_op* next_deferred = nullptr;
  };

  class file {
   public:
    file(sync_context&, const std::filesystem::path& path, bool direct_io)
        : src_(path, direct_io) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return src_.size(); }
    [[nodiscard]] std::string_view name() const noexcept {
      return src_.name();
    }

   private:
    friend class sync_context;
    Source src_;
  };

  sync_context(const reader_context_options&, unsigned) noexcept {}
  sync_context(const sync_context&) = delete;
  sync_context& operator=(const sync_context&) = delete;
  // Nothing is in flight by construction, so there is nothing to drain
  // and no callback to suppress: the queued ops simply never complete.

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   read_op& op) {
    op.buf = buf;
    op.off = off;
    op.src = &f.src_;
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
    try {
      op.src->read_at(op.off, op.buf);
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    op.done(&op, ec);
    return 1;
  }

  void wake() noexcept { waiter_.wake(); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return queued_; }

 private:
  deferred_ops<read_op> queue_;
  std::size_t queued_ = 0;
  poll_waiter waiter_;
};

template <class B>
concept writer_backend =
    // Creates/truncates the file, preallocates if asked (fallocate /
    // SetEndOfFile+VDL / F_PREALLOCATE), and engages what it can of the
    // fast path. The unsigned is the engine's queue depth.
    std::constructible_from<B, const std::filesystem::path&,
                            const file_writer_options&, unsigned> &&
    requires(B b, const B cb, unsigned slot, std::uint64_t off,
             std::span<const std::byte> buf, std::uint64_t written) {
      { cb.name() } noexcept -> std::convertible_to<std::string_view>;
      // The reader's degradation question, minus the offset: the engine
      // writes strictly sequentially and only the final submit may be
      // unaligned, so the length alone decides. False routes the buffer
      // to write_sync.
      { cb.wants_async(std::size_t{}) } noexcept -> std::same_as<bool>;
      // Begins an async write of buf at off, owned by `slot`; legal only
      // after wants_async() said yes for this length. Returns without
      // waiting: the device drains while the producer fills the next
      // buffer.
      { b.start_write(slot, off, buf) };
      // Blocks until `slot` is idle (trivially so on sync backends).
      // This is the engine's backpressure point: acquire() calls it
      // before recycling the slot's buffer. Throws the slot's deferred
      // write error as std::system_error.
      { b.wait_slot(slot) };
      // Positional synchronous write: completes fully or throws. Handle
      // choice (direct vs buffered) for the length's alignment is
      // backend business, same as read_sync.
      { b.write_sync(off, buf) };
      // End of stream: drain every in-flight write, trim the
      // preallocation back to `written` bytes, flush what needs
      // flushing. May be called more than once; the destructor is the
      // error-swallowing fallback for what finish() didn't get to.
      { b.finish(written) };
    };

}  // namespace blake3pp::detail::io_impl
