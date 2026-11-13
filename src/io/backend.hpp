#pragma once

// The portable contract between the engines and the per-OS I/O
// backends. Reads and writes share it: an io_context owns the completion
// mechanism, a file is opened against it, and every transfer is named by
// a caller-owned operation whose callback reports it.
//
// Three rules span every operation:
//  - Teardown drain: a context's destructor drains every in-flight
//    operation, running no callback. A file's destructor that closes with
//    transfers owed does the same, either by draining the whole context
//    (uring, aio, sync_context) or by forget()ting its own transfers
//    (iocp, ioring, gcd). The engines declare their buffer pool member
//    before the context, so the drain runs before the pool is freed.
//  - Operation lifetime: an operation must outlive the drain of its file
//    and of its context. A lane or a dispatch worker may be finishing a
//    transfer until that drain returns, and it writes the result into the
//    operation; a queued operation is read when a lane takes it. Destroy
//    the file (or call drain()/forget()) first, then the operations.
//  - Callback discipline: a transfer's callback runs only inside poll(),
//    on the thread that called poll(). Never from submit_read(),
//    submit_write(), flush(), wake() or a destructor, and never on a lane
//    or another backend thread. The pipelines (several windows, then
//    several files, driven as senders from one thread) depend on there
//    being exactly one place where caller code regains control.
//    forget(file&) is the one exception, and only for a sibling's
//    transfer: where the completion mechanism is shared (one ring, one
//    port, one kqueue), waiting out this file's transfers can reap
//    another file's, and that completion is reported so its operation is
//    not stranded. It runs on the polling thread, from a call the pipeline
//    makes while its own cells are alive. A file destructor that calls
//    forget() can therefore run a sibling's callback.
// Internal to src/io/, never installed.

#include <atomic>
#include <cerrno>
#include <string_view>
#if defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#endif
#include <condition_variable>
#include <mutex>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/mman.h>  // madvise(MADV_HUGEPAGE) for the arena
#endif
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/file_writer.hpp>

#include "io/engine_rung.hpp"

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_errno(const char* what) {
  // Read before the throw: the exception object is allocated first, and
  // errno must be the failed call's, not the allocator's.
  const int e = errno;
  throw std::system_error(e, std::generic_category(), what);
}

// The O_DIRECT / FILE_FLAG_NO_BUFFERING buffer-and-length granule: the
// alignment callers plan their reads around, from driver.hpp.
constexpr std::size_t direct_align = direct_alignment;

// The buffer arena of the engines and the driver: one direct-I/O-aligned
// allocation behind an owning pointer, so member declaration order alone
// sequences its release against the backend's drain.
struct aligned_delete {
  void operator()(std::byte* p) const noexcept {
    ::operator delete(p, std::align_val_t{direct_align});
  }
};
using aligned_buffer = std::unique_ptr<std::byte[], aligned_delete>;

// The arena is virtual until the first pass of a run touches it, so every
// page is a fault paid inside the pipeline, 262144 of them per GiB at
// 4 KiB. Transparent huge pages make that pass cheaper where the kernel
// offers them. Touching 1 GiB a page at a time on Zen 3+ measured
// 550-740 ms as plain pages against 216-221 ms advised.
//
// End to end, where the hash is the limit, it measured 2-4% on a warm
// 2 GiB file at a 1 GiB arena, within that machine's noise. Runs where
// the device outruns the hash, which would pay the faults in the open,
// have not been measured.
//
// A hint, so a kernel without THP or set to `never` refuses it and nothing
// changes; only asked above one huge page, 2 MiB everywhere it exists.
[[nodiscard]] inline aligned_buffer make_aligned_buffer(std::size_t bytes) {
  aligned_buffer buf(static_cast<std::byte*>(
      ::operator new(bytes, std::align_val_t{direct_align})));
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  if (bytes >= std::size_t{2} << 20) {
    (void)::madvise(buf.get(), bytes, MADV_HUGEPAGE);
  }
#endif
  return buf;
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
  // Writes with a preallocation, Windows only: declare the preallocated
  // range valid without zeroing it (SetFileValidData), which needs
  // SeManageVolumePrivilege and enables it on the process token. Where
  // that cannot be had the file is made sparse instead, which is also
  // what a write without this does; see iocp_backend.hpp. Unix file
  // systems preallocate unwritten extents that read as zeros for free,
  // so the flag means nothing there.
  bool waive_zero_fill = false;
};

// What a context needs to know at construction; the engine fills it from
// file_reader_options. A type of its own because the window size and
// queue depth are the engine's, and a context outlives any one file's
// options once several files share it.
struct reader_context_options {
  bool async = true;           // try the OS's async engine at all
  bool offload_submit = true;  // io_uring: issue on io-wq (IOSQE_ASYNC)
  // Windows: prefer the I/O ring over the completion port where the
  // machine has one (Windows 11, IORING_VERSION_3). Off selects the
  // port, which is the fallback anyway; the file's name() says which ran.
  bool ioring = true;
};

// The wake primitive behind the poll(block) of the uring, gcd and
// synchronous contexts, and of an aio context without a kqueue. wake() is
// the only member of a context another thread may call: the caller's
// other threads use it to get a blocked poll() back, and the lanes use it
// to report a finished transfer. A wake issued while nobody is blocked is
// remembered, and the next sleep returns at once, so a wake that races a
// poll is not lost.
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

// One transfer in flight, a read or a write. The caller owns it and keeps
// it at a fixed address from the submit until its callback has run;
// backends derive their own op type from this and recover it from whatever
// the OS hands back (io_uring's user_data, IOCP's OVERLAPPED*). `done` is
// called exactly once per submitted transfer, with an empty error_code on
// success.
struct transfer_base {
  void (*done)(transfer_base*, std::error_code) noexcept = nullptr;
  void* owner = nullptr;  // for the caller's use
};

// device_time, the one the drivers report, is declared in
// detail/driver.hpp because it crosses the pimpl. It is stamped by
// whoever performs a transfer, the only place the device's own time can
// be told apart from a span of the pipeline's timeline: a lane stamps
// around its own pread, which excludes its wait for a lane, and an
// asynchronous engine stamps from submission to completion.


// What a transfer needs while a mechanism is performing it. Every context
// that performs transfers derives its transfer from this.
//
// Kept out of transfer_base because windows_context::transfer is a
// transfer_base around a union of the ring and the port transfers, each a
// transfer_base of its own (backend_select.hpp): a byte added to the base
// would be charged twice there. The composite only forwards the transfer
// the chosen backend performed, so it needs none of this.
struct performed_transfer : transfer_base {
  // The device's own time over this transfer, summed over the pieces it
  // was carried in, and the error the performer saw.
  //
  // One accumulator and no start stamp: beginning a piece subtracts the
  // clock and ending it adds the clock back, so what is left is the sum
  // of the stretches. Eight bytes instead of sixteen keeps the Windows
  // union inside io_op's storage.
  void device_begin() noexcept {
    if (!charging) {
      charging = true;
      device_ns -= steady_now_ns();
    }
  }
  // Ends the charge, whether the piece landed or was taken back unsent.
  // Idempotent, so a backend may call it wherever a piece leaves the
  // mechanism's hands without tracking which of those places it was.
  void device_end() noexcept {
    if (charging) {
      charging = false;
      device_ns += steady_now_ns();
    }
  }
  // What the device had this transfer for, for device_time::add(). Valid
  // once no piece is outstanding, which is what reporting a transfer
  // means.
  [[nodiscard]] std::uint64_t device_total_ns() const noexcept {
    return device_ns > 0 ? static_cast<std::uint64_t>(device_ns) : 0;
  }

  // The performer's result, as four bytes and a flag instead of the
  // sixteen a std::error_code takes. Two categories reach here: errno,
  // from every POSIX source and every cancel, and the Windows system
  // category. A code from any other category keeps its value and reads
  // back in the generic category.
  void set_result(std::error_code ec) noexcept {
    result_value = ec.value();
    result_is_system = ec.category() == std::system_category();
  }
  [[nodiscard]] std::error_code result() const noexcept {
    if (result_value == 0) {
      return {};
    }
    return result_is_system
               ? std::error_code(result_value, std::system_category())
               : std::error_code(result_value, std::generic_category());
  }

  // Back to the state a fresh submit starts from. Called where a backend
  // arms a transfer, since one operation serves transfer after transfer.
  void reset_performance() noexcept {
    device_ns = 0;
    result_value = 0;
    charging = false;
    result_is_system = false;
  }

  std::int64_t device_ns = 0;
  std::int32_t result_value = 0;
  bool charging = false;          // a piece is outstanding
  bool result_is_system = false;  // the category of result_value
};

// The completion-callback I/O contract.
//
// A context owns the completion mechanism (a ring, a port, a queue) and
// nothing about any particular file. C::file is an open file bound to a
// context: constructed from (C&, const std::filesystem::path&,
// const file_open_options&) and exposing size() and name(). A source that
// has no path constructs its file differently -- the tests' fake_context
// takes a size -- which is why construction is described here rather than
// required below.
//
// name() is where the degradation ladder becomes visible: it is settled
// when the file is opened, against the context that opened it, and
// file_reader::backend() reports it verbatim.
//
// Lifetime: every C::file must be destroyed before its context, and the
// context's destructor drains every in-flight transfer without running
// callbacks. Every operation must outlive both drains: the file's, which
// its destructor runs when transfers are owed, and the context's. The
// engines' member order enforces all three.
//
// Errors reach the callback as std::error_code:
//  - errno values in the generic category;
//  - Windows errors in the system category;
//  - a cancel as operation_canceled, an unexpected EOF as EIO.
// poll() throws only when the mechanism itself fails, never for a
// transfer's own error.
//
// A transfer that in_flight() counts must reach its callback, so a
// refused submission is a degradation, not an exception: the context
// moves the transfer to its deferred list, where a lane performs it
// (poll() does, when no lane runs) and poll() reports the result. An
// exception at that point would leave the transfer counted, uncompleted
// and unreachable.
template <class C>
concept io_context = requires(C c, const C cc, typename C::file& f,
                              typename C::transfer& op, std::uint64_t off,
                              std::span<std::byte> buf,
                              std::span<const std::byte> cbuf, bool block,
                              std::uint64_t bytes) {
  typename C::file;
  typename C::transfer;
  requires std::derived_from<typename C::transfer, transfer_base>;
  requires std::default_initializable<typename C::transfer>;
  // The file_reader::backend() string, computed when the file was opened
  // and owned by it: the engine hands it straight to file_reader.
  { f.name() } noexcept -> std::same_as<std::string_view>;
  // The file's length as the open found it, which the engines and the
  // driver read to plan and to bound every transfer.
  { f.size() } noexcept -> std::same_as<std::uint64_t>;
  // Queues one read of exactly buf.size() bytes at off. Performs no
  // transfer, runs no callback and cannot fail. A transfer the asynchronous engine
  // cannot take (no engine, a length NO_BUFFERING cannot take on Windows,
  // a zero length) goes on the context's deferred list, where a lane
  // reads it (poll() does, when no lane runs).
  { c.submit_read(f, off, buf, op) } noexcept;
  // Queues one write of exactly cbuf.size() bytes at off, under the same
  // rules as submit_read. The caller's bytes must stay put until the
  // callback runs.
  { c.submit_write(f, off, cbuf, op) } noexcept;
  // Sets the file's size, for trimming a preallocation back to what was
  // actually written. Only meaningful on a file opened for writing.
  { f.set_size(bytes) };
  // Pushes everything queued to the OS in one call. Never throws: what
  // the OS refuses moves to the deferred list, still owed.
  { c.flush() } noexcept;
  // Flushes, reaps completions, reissues short transfers, runs the
  // callbacks of every transfer the engine or the lanes finished, and
  // performs at most one deferred transfer itself when no lane runs.
  // With block == true, sleeps until at least one callback ran or wake()
  // was called. Returns the number of callbacks run.
  { c.poll(block) } -> std::same_as<std::size_t>;
  // The only member callable from another thread: makes a blocked poll()
  // return. The caller's other threads use it to get the polling thread
  // back, and the lanes use it to report a finished transfer.
  // Idempotent; a wake with no blocked poll is remembered.
  { c.wake() } noexcept;
  { cc.in_flight() } noexcept -> std::same_as<std::size_t>;
  // Waits out every transfer the kernel, a lane or a dispatch worker
  // holds and forgets the rest, running no callback: afterwards
  // in_flight() is zero and the context references no transfer, so the
  // caller may destroy the operations. Legal only while those operations
  // are alive. The context stays usable, and its destructor's drain is
  // then a no-op. Never throws.
  { c.drain() } noexcept;
  // The same for one file: afterwards the context references no transfer
  // of this file and in_flight() counts none of them, so the file and the
  // buffers its transfers named may be destroyed. Legal only while those
  // operations are alive. Runs no callback of this file's. A sibling
  // file's completion that surfaces during the wait is reported, or left
  // for a later poll(), and never dropped. Every file destructor calls
  // this.
  { c.forget(f) } noexcept;
  // Asks the OS to abandon one transfer. Advisory, and never a
  // completion in itself: the op is still owed its callback and still
  // counted by in_flight(), and the callback arrives from a later poll().
  // It reports whichever happened first: operation_canceled if the
  // cancel won, the data if the transfer did, or the transfer's own
  // error. A transfer a lane has already taken runs to completion.
  //
  // Legal only from the thread that calls poll(), and only for an op
  // that was submitted and whose callback has not run. A backend with no
  // cancel mechanism may do nothing: the transfer runs to completion and
  // reports normally.
  { c.cancel(op) } noexcept;
  // The most transfers this context can have at the device at once: the
  // caller's depth where an asynchronous engine took them, and otherwise
  // the number of lanes serving the deferred list (one on the completion
  // port's synchronous rung). Never zero. diagnose_read() and
  // diagnose_write() read the device's queue against it, so a rung that
  // serves two transfers at a time is not reported as a full queue of
  // eight.
  { cc.device_depth() } noexcept -> std::same_as<unsigned>;
  // Nanoseconds transfers spent waiting for a lane before they started,
  // with none of the device in it: the measure of too few lanes, as
  // opposed to a device that is the wall. Zero on a context that served
  // everything through its asynchronous engine.
  { cc.lane_wait_ns() } noexcept -> std::same_as<std::uint64_t>;
  // What the device itself took over this run, stamped by whoever
  // performed each transfer: a lane around its own pread, an engine from
  // submission to completion. A wait for a lane is never part of it. Zero
  // transfers means this context cannot say.
  { cc.device() } noexcept -> std::same_as<device_time>;
};

// An intrusive FIFO of operations linked through their next_deferred:
// the queue inside deferred_pump, and the aio backend's list of requests
// not yet offered to the kernel. Not synchronized; deferred_pump guards
// its own under a mutex.
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

  // The oldest queued op, left queued; the rest follow its
  // next_deferred. For a backend that OFFERS a batch to the OS and only
  // takes off the queue what the OS accepted: reading the queue and
  // emptying it have to be separable for that, since a refusal must
  // leave the order as it was.
  [[nodiscard]] Op* front() const noexcept { return head_; }

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

// --- Blocking-I/O lanes, and the deferred list that rides them -------------

// A lane is one thread that performs blocking transfers, so the thread
// that calls poll() does not have to.
//
// The pipeline admits windows, returns finished windows to the pool and
// feeds the reducer between two polls, so a whole window's pread inside
// poll() stalls all of it for as long as the device takes. With the
// pread inside poll(), 855 ms of a 1040 ms run was finished hash work
// waiting for the polling thread, against 8 ms through io_uring on the
// same file.
//
// A lane performs the transfer and nothing else. The callback still runs
// only inside poll(), on the thread that called it.

// The most lanes one context runs.
//
// One lane takes the transfer off the polling thread. Each further lane
// is another transfer the device can have at once on a rung that would
// otherwise see exactly one, whatever the queue depth says.
//
// Four, because the transfers here are whole windows (8 MiB by default)
// and large sequential transfers stop gaining at a shallow depth: 64 MiB
// writes at depth 2 and depth 4 measured within 2% of each other, 24.2
// against 23.8 GiB/s on four PCIe 5 drives (cli/gen.cpp). Each lane is a
// thread the hash pool, already sized to the machine, does not get.
inline constexpr unsigned max_io_lanes = 4;

// Lanes for a context whose caller asked for `max_inflight` transfers at
// once. More lanes than transfers can never help, so the depth is a
// ceiling as much as the constant is.
[[nodiscard]] inline unsigned io_lanes_for(unsigned max_inflight) noexcept {
  return std::clamp(max_inflight, 1u, max_io_lanes);
}

// Owns a few lanes and the rules for owning them, and nothing about what
// they carry: the deferred pump (below) runs a queue of discrete transfers
// over them.
//
// Every member is for the polling thread except stopping() and
// wake_poller(), which are what a lane's own body calls.
class io_lanes {
 public:
  // The context's wake(), its one member another thread may call. A lane
  // releases a blocked poll() through it, through whatever that context
  // blocks on: an eventfd, a kqueue user event, a completion port, an
  // event handle.
  using wake_fn = void (*)(void*) noexcept;
  // One lane's whole life. It returns once stopping() reads true.
  using body_fn = void (*)(void*) noexcept;
  // How stop() gets a lane out of whatever it waits on: a condition
  // variable notify. Called after the stop flag is set and before the
  // join, if start() has been given one.
  using unblock_fn = void (*)(void*) noexcept;

  // Spells a context's wake() as one of those:
  //   lanes_(io_lanes::waker<my_context>(), this, io_lanes_for(depth))
  template <class Ctx>
  [[nodiscard]] static constexpr wake_fn waker() noexcept {
    return +[](void* p) noexcept { static_cast<Ctx*>(p)->wake(); };
  }

  // `owner` must outlive this. Every consumer declares this as a member
  // of the owner, so the destructor's join runs before the owner goes.
  io_lanes(wake_fn wake, void* owner, unsigned lanes) noexcept
      : wake_(wake), owner_(owner), want_(std::max(1u, lanes)) {}

  io_lanes(const io_lanes&) = delete;
  io_lanes& operator=(const io_lanes&) = delete;

  // Joins. A lane writes into a buffer the caller owns, so nothing
  // holding one of those buffers may be released before this returns.
  // Each consumer declares this after everything a lane touches.
  ~io_lanes() { stop(); }

  // Starts the lanes, once, on first use: a context whose engine took
  // everything never creates a thread, and setup that throws before the
  // first transfer leaves none behind.
  //
  // False means not one lane could be started, and the caller performs
  // the work on the polling thread instead, so a machine that will not
  // give a thread still completes a transfer it already accepted.
  bool start(body_fn body, unblock_fn unblock, void* cookie) noexcept {
    if (started_) {
      return !lanes_.empty();
    }
    started_ = true;
    unblock_ = unblock;
    cookie_ = cookie;
    try {
      lanes_.reserve(want_);
    } catch (...) {  // not one lane, which the result reports
      return false;
    }
    for (unsigned i = 0; i < want_; ++i) {
      try {
        lanes_.emplace_back([body, cookie] { body(cookie); });
      } catch (...) {  // NOLINT(bugprone-empty-catch): reported by the result
        break;
      }
    }
    return !lanes_.empty();
  }

  // Whether start() has run and started at least one lane.
  [[nodiscard]] bool running() const noexcept {
    return started_ && !lanes_.empty();
  }

  // Lanes that exist, or the number start() will ask for. Never zero, so
  // a caller dividing by it does not have to check: a context with no
  // lanes serves one transfer at a time on the polling thread.
  [[nodiscard]] unsigned count() const noexcept {
    return started_ ? std::max(1u, static_cast<unsigned>(lanes_.size()))
                    : want_;
  }

  // A lane's body returns when this reads true.
  [[nodiscard]] bool stopping() const noexcept {
    return stopping_.load(std::memory_order_acquire);
  }

  // From a lane, when it has something for the polling thread.
  void wake_poller() noexcept { wake_(owner_); }

  // Flag, unblock, join. Idempotent, and what the destructor does.
  void stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (unblock_ != nullptr) {
      unblock_(cookie_);
    }
    for (std::thread& t : lanes_) {
      if (t.joinable()) {
        t.join();
      }
    }
    lanes_.clear();
  }

 private:
  wake_fn wake_;
  void* owner_;
  unblock_fn unblock_ = nullptr;
  void* cookie_ = nullptr;
  unsigned want_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
  std::vector<std::thread> lanes_;
};

// Performs, on io_lanes, the transfers a context could not hand to its
// asynchronous engine: a piece NO_BUFFERING cannot take, a submission the
// kernel refused, or no engine at all.
//
// A queue of discrete transfers of known length, completing in any order,
// which is what a pipeline submits. A source whose read decides its own
// length cannot be one of these and takes a lane of its own.
//
// The operation type needs `next_deferred`, `cancelled` and what
// performed_transfer carries. One link serves both the queue and the done
// list, because an operation is on exactly one of them.
template <class Op>
class deferred_pump {
 public:
  // Performs one transfer and says how it went. Runs on a lane, so it may
  // touch nothing but the operation and the file the operation names.
  // noexcept: a lane has no caller to catch an exception.
  using perform_fn = std::error_code (*)(Op&) noexcept;

  deferred_pump(io_lanes::wake_fn wake, void* owner, perform_fn perform,
                unsigned lanes) noexcept
      : perform_(perform), lanes_(wake, owner, lanes) {}

  deferred_pump(const deferred_pump&) = delete;
  deferred_pump& operator=(const deferred_pump&) = delete;

  // Hands one transfer over. From the polling thread only.
  void push(Op& op) noexcept {
    std::unique_lock<std::mutex> lock(m_);
    if (!lanes_.running()) {
      lanes_.start(&lane_body, &lane_unblock, this);
    }
    accrue();
    queue_.push(op);
    ++queued_;
    if (!lanes_.running()) {
      return;  // the polling thread will serve it: see run_one_here()
    }
    lock.unlock();
    work_.notify_one();
  }

  // Every transfer the lanes have finished, as one list linked through
  // next_deferred, LIFO -- which the contract permits, since a caller
  // matches a completion to its window by the operation and not by order.
  // Each carries its result in result(). From the polling thread
  // only.
  [[nodiscard]] Op* take_done() noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return std::exchange(done_, nullptr);
  }

  // Serves one queued transfer on the CALLING thread, and only for a
  // context that got no lanes. Returns the operation, performed, or null.
  // From the polling thread only.
  [[nodiscard]] Op* run_one_here() noexcept {
    Op* op = nullptr;
    bool cancelled = false;
    {
      const std::lock_guard<std::mutex> lock(m_);
      if (lanes_.running() || queue_.empty()) {
        return nullptr;
      }
      op = queue_.take();
      accrue();
      --queued_;
      cancelled = op->cancelled;
    }
    perform_one(perform_, *op, cancelled);
    return op;
  }

  // Flags a transfer no lane has taken yet, which is the whole of a cancel
  // here: once a lane has one the flag arrives too late and the transfer
  // runs to completion, which is what the contract's advisory cancel
  // allows. Under the lock, because the flag is what a lane reads when it
  // takes the transfer.
  void cancel(Op& op) noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    op.cancelled = true;
  }

  [[nodiscard]] bool queue_empty() const noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return queue_.empty();
  }

  // Forgets the queued transfers `drop` accepts -- for a file closing with
  // transfers it never issued, which name its handles and its share of
  // the count. None was ever performed, so there is nothing to wait for
  // and none gets its callback. Returns how many.
  template <class Pred>
  std::size_t remove_queued_if(Pred drop) noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    accrue();
    const std::size_t dropped = queue_.remove_if(drop);
    queued_ -= std::min(dropped, queued_);
    return dropped;
  }

  // The same for transfers the lanes finished but nobody has reported.
  // Call it after quiesce(), or a transfer still in a lane's hands lands
  // on the list behind this sweep.
  template <class Pred>
  std::size_t remove_done_if(Pred drop) noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    std::size_t dropped = 0;
    Op** link = &done_;
    while (*link != nullptr) {
      Op* const op = *link;
      if (drop(*op)) {
        *link = op->next_deferred;
        op->next_deferred = nullptr;
        ++dropped;
      } else {
        link = &op->next_deferred;
      }
    }
    return dropped;
  }

  // Waits until no lane holds a transfer, so the buffers they were
  // writing into may be released. It does NOT stop a lane picking up what
  // is still queued: a caller reclaiming one file clears that file's
  // queue first, and a caller draining the context clears the whole
  // queue first.
  void quiesce() noexcept {
    std::unique_lock<std::mutex> lock(m_);
    idle_.wait(lock, [this] { return busy_ == 0; });
  }

  // Forgets everything queued and everything finished, reporting none of
  // it: the context-wide teardown drain. Waits the lanes out in the
  // middle, because a transfer one of them holds is writing into memory
  // the caller is about to reuse.
  void clear() noexcept {
    {
      const std::lock_guard<std::mutex> lock(m_);
      accrue();
      queue_.clear();
      queued_ = 0;
    }
    quiesce();
    const std::lock_guard<std::mutex> lock(m_);
    done_ = nullptr;
  }

  // The transfers this has at the device at once, which is the device's
  // queue depth on this rung.
  [[nodiscard]] unsigned depth() const noexcept { return lanes_.count(); }

  // Summed time transfers spent queued here waiting for a lane, with none
  // of the device in it: the measure of too few lanes, as opposed to a
  // device that is the wall.
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    // Whatever is queued right now has been waiting since the last
    // change, and a caller reading this at the end of a run has usually
    // just drained: accrue that stretch too rather than losing it.
    return lane_wait_ns_ + waiting_since_change();
  }

 private:
  static void lane_body(void* p) noexcept {
    static_cast<deferred_pump*>(p)->run();
  }

  // Takes the lock before notifying, so a lane cannot be between its
  // predicate and its wait when the stop flag is set.
  static void lane_unblock(void* p) noexcept {
    auto* const self = static_cast<deferred_pump*>(p);
    { const std::lock_guard<std::mutex> lock(self->m_); }
    self->work_.notify_all();
  }

  // The transfer, charged to the device around itself: the wait for a
  // lane is already over, so this measures the device alone. Two clock
  // reads per transfer.
  static void perform_one(perform_fn perform, Op& op,
                          bool cancelled) noexcept {
    if (cancelled) {
      op.set_result(std::make_error_code(std::errc::operation_canceled));
      return;
    }
    op.device_begin();
    op.set_result(perform(op));
    op.device_end();
  }

  void run() noexcept {
    for (;;) {
      Op* op = nullptr;
      bool cancelled = false;
      {
        std::unique_lock<std::mutex> lock(m_);
        work_.wait(lock, [this] {
          return lanes_.stopping() || !queue_.empty();
        });
        if (lanes_.stopping()) {
          // Queued transfers are the caller's to forget: a stop only
          // happens after a drain, which has cleared them, and performing
          // one now would write into memory that may already be gone.
          return;
        }
        op = queue_.take();
        accrue();
        --queued_;
        cancelled = op->cancelled;
        ++busy_;
      }
      perform_one(perform_, *op, cancelled);
      {
        const std::lock_guard<std::mutex> lock(m_);
        op->next_deferred = done_;
        done_ = op;
        if (--busy_ == 0) {
          idle_.notify_all();
        }
      }
      lanes_.wake_poller();
    }
  }

  // What everything currently queued has waited since the length last
  // changed. Under the lock.
  [[nodiscard]] std::uint64_t waiting_since_change() const noexcept {
    if (queued_ == 0) {
      return 0;
    }
    const std::int64_t d = steady_now_ns() - since_;
    return d > 0 ? static_cast<std::uint64_t>(d) * queued_ : 0;
  }

  // Little's law, kept incrementally: the summed wait of every transfer
  // that passes through the queue is the integral of the queue's length
  // over time. So one clock read per change of length is enough and no
  // transfer needs a stamp of its own. Under the lock, before the length
  // changes.
  void accrue() noexcept {
    lane_wait_ns_ += waiting_since_change();
    since_ = steady_now_ns();
  }

  mutable std::mutex m_;
  std::condition_variable work_;  // a transfer was queued, or stop() ran
  std::condition_variable idle_;  // the last busy lane finished
  deferred_ops<Op> queue_;
  Op* done_ = nullptr;
  perform_fn perform_;
  std::size_t queued_ = 0;
  std::size_t busy_ = 0;
  std::uint64_t lane_wait_ns_ = 0;
  std::int64_t since_ = 0;
  // Declared last of the state they touch, and joined by its own
  // destructor before any of it goes.
  io_lanes lanes_;
};

// The context of every synchronous source. submit_read() and
// submit_write() only queue. A lane of the pump performs the transfer,
// and poll() runs the callbacks of what the lanes finished. The source
// opens a path, knows its size and name, reads and writes a span at an
// offset, and sets the size. pread and stdio differ only there.
//
// A source says whether it can serve two transfers at once through
// Source::positional. pread and pwrite can, because they carry their own
// offset. stdio cannot, because seek-then-read is two calls over one
// cursor. That source gets a single lane, and its transfers run one at a
// time.
template <class Source>
class sync_context {
  static_assert(std::same_as<decltype(Source::positional), const bool>,
                "a synchronous source must declare whether its transfers "
                "may run concurrently: static constexpr bool positional");

 public:
  class file;

  struct transfer : performed_transfer {
    std::span<std::byte> buf{};        // reads fill this
    std::span<const std::byte> wbuf{};  // writes read from this
    std::uint64_t off = 0;
    Source* src = nullptr;
    file* owner_file = nullptr;  // whose transfer this is, for forget()
    transfer* next_deferred = nullptr;
    bool cancelled = false;
    bool is_write = false;
  };

  class file {
   public:
    file(sync_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : ctx_(&ctx), src_(path, how) {}

    // Queued transfers name this file, so they go with it, and only
    // they: forget() drops this file's queued and finished transfers and
    // waits out the lanes, and a sibling's stay to be reported. The
    // operations must still be alive here (see the contract's operation
    // lifetime rule): a lane may be writing into one until forget()
    // returns. A count a context-wide drain left stale costs one such
    // wait and nothing else.
    ~file() {
      if (inflight_ != 0) {
        ctx_->forget(*this);
      }
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return src_.size(); }
    [[nodiscard]] std::string_view name() const noexcept {
      return src_.name();
    }
    void set_size(std::uint64_t bytes) { src_.set_size(bytes); }

   private:
    friend class sync_context;
    sync_context* ctx_;
    Source src_;
    // This file's share of the context's count, so the destructor can tell
    // whether anything of its own is queued without walking the queue.
    std::size_t inflight_ = 0;
  };

  // The rung this is, declared for the ladder walk in
  // src/io/io_engines.cpp. A synchronous context has nothing under it and
  // nothing can refuse it, so its verdict is always settled. The source
  // names the engine, as its name() spells the file's backend string.
  static constexpr io_engine rung_engine = Source::rung_engine;
  using rung_below = void;
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return {rung_engine, true};
  }

  // The depth the caller asked for decides how many threads serve the
  // queue, since more threads than transfers in flight can never help.
  // A source that cannot take two at once gets one whatever the depth.
  sync_context(const reader_context_options&, unsigned max_inflight) noexcept
      : pump_(io_lanes::waker<sync_context>(), this, &perform,
              Source::positional ? io_lanes_for(max_inflight) : 1u) {}
  sync_context(const sync_context&) = delete;
  sync_context& operator=(const sync_context&) = delete;
  // Drains before any member goes, so no lane is inside a transfer once
  // this returns and none takes another: the operations, which the
  // contract keeps alive until here, are not touched after it. The
  // pump's own destructor then joins its idle lanes; it is declared last,
  // so the waiter its lanes signal outlives the join. Every file must
  // already be gone, and each one's destructor drained what it owed.
  ~sync_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    op.buf = buf;
    op.wbuf = {};
    op.off = off;
    op.src = &f.src_;
    op.owner_file = &f;
    op.cancelled = false;
    op.is_write = false;
    op.reset_performance();
    in_flight_++;
    f.inflight_++;
    pump_.push(op);
  }

  // Handed to the pump like a read; a lane performs it and the callback
  // runs inside poll().
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, transfer& op) noexcept {
    op.buf = {};
    op.wbuf = buf;
    op.off = off;
    op.src = &f.src_;
    op.owner_file = &f;
    op.cancelled = false;
    op.is_write = true;
    op.reset_performance();
    in_flight_++;
    f.inflight_++;
    pump_.push(op);
  }

  // The pump was given the transfer when it was submitted, so there is
  // no batch to push. The contract keeps the call because io_uring has
  // one.
  void flush() noexcept {}

  // Gives up this file's transfers without reporting them. What is still
  // queued was never performed and is dropped; what a lane holds is
  // waited out, because it is reading or writing through this file's
  // descriptor and into the caller's buffer. Legal only while the
  // operations are alive; the file's destructor calls it.
  //
  // The lanes are the context's, so the wait covers sibling files too,
  // and a sibling's finished transfer stays on the done list for a later
  // poll() to report.
  void forget(file& f) noexcept {
    const auto mine = [&f](const transfer& op) noexcept {
      return op.owner_file == &f;
    };
    std::size_t dropped = pump_.remove_queued_if(mine);
    pump_.quiesce();
    dropped += pump_.remove_done_if(mine);
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
  }

  std::size_t poll(bool block) {
    std::size_t ran = serve_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (waiter_.take()) {
        // A wake from another thread or from a lane; the done list says
        // which.
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

  // A transfer no lane has taken yet is flagged and reports
  // operation_canceled; one a lane already has runs to completion.
  void cancel(transfer& op) noexcept { pump_.cancel(op); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Waits the lanes out, then forgets everything queued and everything
  // finished: none of it is reported. The files' own counts are left as
  // they were; see ~file().
  void drain() noexcept {
    pump_.clear();
    in_flight_ = 0;
  }

  // Every transfer rides the pump here, so its lanes ARE the device's
  // queue and its stamps are the whole of the device's time.
  [[nodiscard]] unsigned device_depth() const noexcept {
    return pump_.depth();
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }

 private:
  // One transfer, on a lane. Touches the operation and the source it
  // names, and nothing else of this context.
  static std::error_code perform(transfer& op) noexcept {
    try {
      if (op.is_write) {
        op.src->write_at(op.off, op.wbuf);
      } else {
        op.src->read_at(op.off, op.buf);
      }
    } catch (const std::system_error& e) {
      return e.code();
    } catch (...) {
      // A source throws only system_error; anything else is reported as
      // EIO so it cannot terminate a noexcept lane.
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  // Runs the callbacks of every transfer the lanes finished, on this
  // thread. The inline step serves a context whose lanes could not start.
  std::size_t serve_deferred() noexcept {
    std::size_t ran = 0;
    for (transfer* op = pump_.take_done(); op != nullptr;) {
      transfer* const next = op->next_deferred;
      report(*op);
      ++ran;
      op = next;
    }
    if (transfer* const op = pump_.run_one_here(); op != nullptr) {
      report(*op);
      ++ran;
    }
    return ran;
  }

  void report(transfer& op) noexcept {
    in_flight_--;
    assert(op.owner_file != nullptr &&
           "every transfer is queued with its file");
    op.owner_file->inflight_--;
    device_.add_if_served(op.device_total_ns());
    op.done(&op, op.result());
  }

  std::size_t in_flight_ = 0;
  // What the lanes measured around their own transfers.
  device_time device_{};
  poll_waiter waiter_;
  // Last, so its destructor joins the lanes while the waiter they signal
  // is still alive. Do not move it above waiter_.
  deferred_pump<transfer> pump_;
};

}  // namespace blake3pp::detail::io_impl
