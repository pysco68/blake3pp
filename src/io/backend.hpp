#pragma once

// The portable contract between the file_reader/file_writer engines and
// the per-OS I/O backends. 
//
// Two contract rules that span every operation, so they live here rather
// than on any one requirement below:
//  - Slot exclusivity: start()/start_write() may only be called for a
//    slot the backend claimed via wants_async(...), and only while
//    nothing else is outstanding on that slot.
//  - Teardown drain: a backend's destructor drains every in-flight
//    operation. The engines declare their buffer pool member BEFORE the
//    backend member precisely so the drain runs before the pool is
//    freed.
//
// The reader side is mid-migration. reader_backend below is the original
// contract: one backend per file, reads named by slot, completed inside a
// blocking wait(slot). reader_context is its replacement: the completion
// mechanism is separate from the file, a read is named by a caller-owned
// operation, and completions are reported through callbacks that run in
// poll(). Windows and macOS still use the first; everything else uses the
// second. A third rule governs it:
//  - Callback discipline: a read's callback runs ONLY inside poll(), on
//    the thread that called poll(). Never from submit_read(), flush(),
//    wake() or a destructor. Everything a later phase wants to build on
//    this -- several windows, then several files, driven as senders from
//    one thread -- depends on there being exactly one place where caller
//    code regains control.
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

// The engines' buffer arena: one direct-I/O-aligned allocation, RAII so
// member declaration order alone sequences teardown against the backend.
struct aligned_pool {
  std::byte* data = nullptr;

  explicit aligned_pool(std::size_t bytes)
      : data(static_cast<std::byte*>(
            ::operator new(bytes, std::align_val_t{direct_align}))) {}
  ~aligned_pool() { ::operator delete(data, std::align_val_t{direct_align}); }
  aligned_pool(const aligned_pool&) = delete;
  aligned_pool& operator=(const aligned_pool&) = delete;
};

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
// direct_io) and exposing size(). A source that has no path constructs
// its file differently -- null_context::file takes a size -- which is why
// construction is described here rather than required below.
//
// Lifetime: every C::file must be destroyed before its context, and the
// context's destructor drains every in-flight read WITHOUT running
// callbacks. The engines' member order is what enforces both.
//
// Errors reach the callback as std::error_code: -res in the generic
// category, and an unexpected EOF as EIO. flush() and poll() throw only
// when the mechanism itself fails (io_uring_enter), never for a read's
// own error.
template <class C>
concept reader_context = requires(C c, const C cc, typename C::file& f,
                                  typename C::read_op& op, std::uint64_t off,
                                  std::span<std::byte> buf, bool block) {
  typename C::file;
  typename C::read_op;
  requires std::derived_from<typename C::read_op, read_op_base>;
  requires std::default_initializable<typename C::read_op>;
  // Is the context's async engine running at all? False means every read
  // is served synchronously inside poll().
  { cc.async() } noexcept -> std::same_as<bool>;
  // The file_reader::backend() string for this file on this context.
  { cc.describe(f) } -> std::convertible_to<std::string>;
  // Queues one read of exactly buf.size() bytes at off. No syscall, no
  // callback. A window the file cannot take asynchronously (an unaligned
  // length under O_DIRECT, or no ring at all) goes on the context's
  // deferred list and is read synchronously inside poll(), which is where
  // the old contract's "lazy at delivery" ended up.
  { c.submit_read(f, off, buf, op) };
  // Pushes everything queued to the OS in one call. May throw.
  { c.flush() };
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

   private:
    friend class sync_context;
    Source src_;
  };

  sync_context(const reader_context_options&, unsigned) noexcept {}
  sync_context(const sync_context&) = delete;
  sync_context& operator=(const sync_context&) = delete;
  // Nothing is in flight by construction, so there is nothing to drain
  // and no callback to suppress: the queued ops simply never complete.

  [[nodiscard]] bool async() const noexcept { return false; }

  [[nodiscard]] std::string describe(const file& f) const {
    return std::string(f.src_.name());
  }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   read_op& op) {
    op.buf = buf;
    op.off = off;
    op.src = &f.src_;
    op.next_deferred = nullptr;
    if (tail_ == nullptr) {
      head_ = tail_ = &op;
    } else {
      tail_->next_deferred = &op;
      tail_ = &op;
    }
    queued_++;
  }

  void flush() noexcept {}

  std::size_t poll(bool block) {
    if (head_ == nullptr) {
      // Nothing to do: a blocking poll here is the driver waiting for
      // another thread, which is what wake() is for.
      if (block) {
        while (!waiter_.take()) {
          waiter_.sleep();
        }
      }
      return 0;
    }
    read_op& op = *head_;
    head_ = op.next_deferred;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
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
  read_op* head_ = nullptr;
  read_op* tail_ = nullptr;
  std::size_t queued_ = 0;
  poll_waiter waiter_;
};

template <class B>
concept reader_backend =
    // Opens the file and decides (at runtime, per feature) how much of
    // the requested fast path (direct I/O, async engine) it can actually
    // deliver. The unsigned is the engine's queue depth: the most slots
    // that can ever be outstanding at once.
    std::constructible_from<B, const std::filesystem::path&,
                            const file_reader_options&, unsigned> &&
    requires(B b, const B cb, unsigned slot, std::uint64_t off,
             std::span<std::byte> buf) {
      { cb.size() } noexcept -> std::same_as<std::uint64_t>;
      { cb.name() } noexcept -> std::convertible_to<std::string_view>;
      // The whole runtime-degradation ladder folded into one question the
      // engine asks per window: "may THIS (offset, length) ride your
      // async path?"
      //
      //   - uring and IOCP answer engaged && length aligned, because
      //     O_DIRECT and NO_BUFFERING reject unaligned lengths.
      //   - GCD answers engaged. F_NOCACHE has no alignment contract, so
      //     the tail rides too.
      //   - sync backends answer never.
      //
      // The engine does not learn why. false routes the window to
      // read_sync at delivery time, and that is all it needs.
      //
      // Current backends ignore the offset, since engine windows start at
      // 64 KiB multiples and it is therefore always granule-aligned. It is
      // part of the question because O_DIRECT constrains offset alignment
      // too, and a future engine might not guarantee that.
      { cb.wants_async(off, std::size_t{}) } noexcept -> std::same_as<bool>;
      // Begins an async read of buf at off, owned by `slot`; legal only
      // after wants_async() said yes for exactly this window.
      { b.start(slot, off, buf) };
      // Blocks until `slot`'s read fully completes, reissuing short
      // reads and absorbing OTHER slots' completions when the OS delivers
      // them out of order. Throws std::system_error on failure, including
      // failures a worker thread captured earlier.
      { b.wait(slot) };
      // Positional synchronous read: completes fully or throws. Picks the
      // right handle internally (direct vs buffered) for the length's
      // alignment; the unaligned-tail dance is backend business.
      { b.read_sync(off, buf) };
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
