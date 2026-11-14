#pragma once

// The Windows backend: CreateFileW handles (std::filesystem::path's native
// wide string is the whole reason the public API trades in paths) and an
// I/O completion port as the completion queue. The mapping to the io_uring
// backend is nearly 1:1: one OVERLAPPED per transfer plays the SQE,
// GetQueuedCompletionStatus plays the completion-queue wait, and
// FILE_FLAG_NO_BUFFERING is O_DIRECT (same sector-alignment demands, a
// buffered handle for the unaligned tail). Degrades per feature at
// runtime: no overlapped handle -> synchronous ReadFile/WriteFile on the
// deferred pump's lanes, NO_BUFFERING refused -> buffered. Internal to
// src/io/, never installed.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <string_view>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

#if defined(BLAKE3PP_TEST_IO_SEAMS)
// The test executable compiles this backend, and the IoRing one that
// shares its helpers, with seams and under a name of their own: the
// library's copies, which the same executable links, stay one definition
// each, and nothing shipped carries the seams.
inline namespace test_seams {
// While positive, each read the port is handed is issued at half its
// length, and the count drops: the completion comes back short, as one
// may, and the rest goes through the port again.
inline std::atomic<unsigned> short_port_reads{0};
// While positive, each read the port is handed is refused, as ReadFile
// refuses one for want of resources, and the count drops.
inline std::atomic<unsigned> refuse_port_reads{0};

[[nodiscard]] inline bool take_seam(std::atomic<unsigned>& count) noexcept {
  unsigned n = count.load();
  while (n != 0 && !count.compare_exchange_weak(n, n - 1)) {
  }
  return n != 0;
}
#endif

// The error is read before the exception is built: nothing guarantees
// that constructing one leaves the thread's last-error value alone.
[[noreturn]] inline void throw_winerr(const char* what) {
  const DWORD error = ::GetLastError();
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          what);
}

// A transfer's Win32 error as the contract names it on every platform, so
// a caller can recognise it without asking which OS it is on: a read the
// port reports as aborted is one CancelIoEx reached (operation_canceled),
// and a read past the end of the file is the unexpected EOF every backend
// reports as io_error.
[[nodiscard]] inline std::error_code read_error(DWORD err) noexcept {
  switch (err) {
    case ERROR_OPERATION_ABORTED:
      return std::make_error_code(std::errc::operation_canceled);
    case ERROR_HANDLE_EOF:
      return std::make_error_code(std::errc::io_error);
    default:
      return std::error_code(static_cast<int>(err), std::system_category());
  }
}

// The most one ReadFile, WriteFile or I/O ring entry carries. Their
// lengths are 32-bit, and this is the largest whole number of granules
// below 4 GiB, so a longer piece is cut here and completes short, and its
// rest is issued as any short transfer's is.
inline constexpr std::size_t max_win_piece = 0xFFFF'F000;
static_assert(max_win_piece % direct_align == 0);

// Owns one Win32 HANDLE and closes it exactly once. Win32 spells "no
// handle" two ways (CreateFileW yields INVALID_HANDLE_VALUE, the IOCP
// calls yield nullptr), so both count as empty here and neither is ever
// handed to CloseHandle. Move-only, so a handle can never be owned twice.
class unique_handle {
 public:
  unique_handle() = default;
  explicit unique_handle(HANDLE h) noexcept : h_(h) {}
  unique_handle(const unique_handle&) = delete;
  unique_handle& operator=(const unique_handle&) = delete;
  unique_handle(unique_handle&& other) noexcept
      : h_(std::exchange(other.h_, INVALID_HANDLE_VALUE)) {}
  unique_handle& operator=(unique_handle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.h_, INVALID_HANDLE_VALUE));
    }
    return *this;
  }
  ~unique_handle() { reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return h_; }
  explicit operator bool() const noexcept {
    return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
  }
  void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
    if (*this) {
      ::CloseHandle(h_);
    }
    h_ = h;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Shared Windows file plumbing, the twin of posix_file: the buffered
// handle that always exists, and the optional NO_BUFFERING/OVERLAPPED
// reopen next to it (both are per-open flags, hence a second handle).
// Every handle lives in a unique_handle, which makes the type
// non-copyable by construction and unwinds the whole set when a
// constructor throws part-way through.
//
// `fast` is held ONLY when the reopen genuinely engaged, so it never
// aliases `plain` and no destructor has to test for that.
// Shared by reader and writer, and by both Windows contexts: the
// completion port's files and the I/O ring's make the same open and
// differ only in how they reopen.
struct win_file {
  unique_handle plain;  // always-buffered+sync: unaligned tails, fallback
  unique_handle fast;   // the reopened handle, when one engaged
  bool direct = false;
  bool use_iocp = false;
  DWORD access = 0;  // as open_file() opened it, for the reopen
  DWORD share = 0;
  std::uint64_t size = 0;   // what open_file() found; 0 for a file written
  bool sparse = false;      // made sparse for the run; cleared by set_size()
  bool valid_data = false;  // the valid data length was set up front

  // The handle the fast path should use: the reopened one when it
  // engaged, else the plain one. Borrowed: the caller never closes it.
  [[nodiscard]] HANDLE h() const noexcept {
    return fast ? fast.get() : plain.get();
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    LARGE_INTEGER sz;
    if (::GetFileSizeEx(plain.get(), &sz) == 0) {
      throw_winerr("GetFileSizeEx");
    }
    return static_cast<std::uint64_t>(sz.QuadPart);
  }

  // The open both Windows contexts' files make, before either reopens for
  // its own engine: the plain handle, the size, and for a file being
  // written its preallocation. `access` and `share` are kept for the
  // reopen.
  //
  // FILE_SHARE_READ and nothing else for a read: a digest describes the
  // bytes that were there, so nothing may rewrite or shorten the file
  // while it is being read. The cost is that a test cannot truncate a
  // file the pipeline holds open, which is why the truncation case in
  // tests/file_pipeline.cpp does not run here. A writable open shares
  // write access with ITSELF: the reopen asks for a second GENERIC_WRITE
  // handle on the same path, and two can only coexist if the first one
  // permits it. Without FILE_SHARE_WRITE the reopen fails with a sharing
  // violation every time, and every write silently takes the synchronous
  // buffered rung.
  //
  // Preallocation belongs to the OPEN, not to the finish: a write that
  // extends the file serializes, and with an async direct queue that
  // collapses it to one stalled write at a time. On Windows that is
  // SetEndOfFile, and set_size() trims back afterwards.
  //
  // NTFS then zeroes every byte between the valid data length and a
  // write that lands beyond it, which with several writes in flight cost
  // the writer half the device. Two ways out, in the order the file
  // system allows them:
  //  - the valid data length set to the preallocation up front. It needs
  //    SeManageVolumePrivilege and exposes the old blocks until they are
  //    written, so only on request (waive_zero_fill);
  //  - else a sparse file, whose gaps stay holes that read as zeros, made
  //    plain again by set_size() once the file is complete.
  // The sparse flag has to precede the preallocation for the range to be
  // a hole, and a valid data length can only be set on a file that is not
  // sparse.
  void open_file(const std::filesystem::path& path,
                 const file_open_options& how) {
    access = how.write ? GENERIC_WRITE : GENERIC_READ;
    share = how.write ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
    plain.reset(::CreateFileW(path.c_str(), access, share, nullptr,
                              how.write ? CREATE_ALWAYS : OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr));
    if (!plain) {
      throw_winerr("CreateFileW");
    }
    size = how.write ? 0 : stat_size();
    if (how.write && how.preallocate_bytes > 0) {
      const bool want_valid_data =
          how.waive_zero_fill && enable_manage_volume_privilege();
      if (!want_valid_data) {
        sparse = set_sparse(true);
      }
      if (set_end_of_file(how.preallocate_bytes) && want_valid_data) {
        valid_data = ::SetFileValidData(
                         plain.get(),
                         static_cast<LONGLONG>(how.preallocate_bytes)) != 0;
      }
    }
  }

  // Trims a preallocation back to what was written. Harmless when nothing
  // was preallocated, which is what a reader's file is. A file made
  // sparse for the run is a plain file again from here: every byte below
  // the new end has been written, so there is no hole to keep.
  void set_size(std::uint64_t bytes) {
    if (!set_end_of_file(bytes)) {
      throw_winerr("SetEndOfFile");
    }
    if (sparse && set_sparse(false)) {
      sparse = false;
    }
  }

  // What open_file() did to a file being written, for its name.
  [[nodiscard]] std::string_view suffix() const noexcept {
    return valid_data ? "+validdata" : sparse ? "+sparse" : "";
  }

  // NO_BUFFERING and OVERLAPPED are per-open flags: engage by reopening,
  // keeping the plain handle for unaligned lengths. Best-effort: when
  // nothing can be engaged the plain handle stays in charge with direct
  // and use_iocp still false.
  //
  // Per feature. The two flags are asked for together first, and a
  // refusal drops one and asks again: NO_BUFFERING is a property of the
  // filesystem (SMB and some virtual volumes reject it) while OVERLAPPED is
  // not, and on Windows buffered async measures within noise of direct
  // async (docs/performance.md), so the port is the half to keep.
  //
  // `port` is the context's: every file it opens associates its
  // overlapped handle with that one port, which the file does not own.
  void engage(const wchar_t* path, bool want_direct, bool want_async,
              HANDLE port) noexcept {
    // OPEN_EXISTING on the reopen in both directions: the plain handle has
    // already created and truncated a file being written, and a second
    // CREATE_ALWAYS here would truncate it again -- including any
    // preallocation just set on it.
    if (want_direct && want_async) {
      if (engage_one(path, true, true, port)) {
        return;
      }
      // The port first: per the measurement above, Windows can do
      // without the cache bypass more easily than without the asynchrony.
      if (engage_one(path, false, true, port)) {
        return;
      }
      (void)engage_one(path, true, false, port);
      return;
    }
    if (want_direct || want_async) {
      (void)engage_one(path, want_direct, want_async, port);
    }
  }

  // One rung: reopen with exactly these two flags, attach the port when
  // async was asked for, and take the handle only if both steps worked.
  // Leaves everything untouched on failure, so the caller may ask again
  // for less.
  [[nodiscard]] bool engage_one(const wchar_t* path, bool with_direct,
                                bool with_async, HANDLE port) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (with_direct) {
      flags |= FILE_FLAG_NO_BUFFERING;
    }
    if (with_async) {
      flags |= FILE_FLAG_OVERLAPPED;
    }
    unique_handle cand(::CreateFileW(path, access, share, nullptr,
                                     OPEN_EXISTING, flags, nullptr));
    if (!cand) {
      return false;
    }
    // An unattachable port takes the fast handle down with it: a
    // FILE_FLAG_OVERLAPPED handle cannot serve the synchronous path, so the
    // candidate closes on the way out and the caller asks for a rung
    // without it.
    if (with_async &&
        ::CreateIoCompletionPort(cand.get(), port, 0, 0) == nullptr) {
      return false;
    }
    use_iocp = with_async;
    fast = std::move(cand);
    direct = with_direct;
    return true;
  }

 private:
  // SetFilePointerEx + SetEndOfFile on the PLAIN handle: the fast one
  // may be FILE_FLAG_NO_BUFFERING, whose file pointer has alignment
  // rules of its own, and the plain handle has none.
  [[nodiscard]] bool set_end_of_file(std::uint64_t bytes) noexcept {
    LARGE_INTEGER to;
    to.QuadPart = static_cast<LONGLONG>(bytes);
    if (::SetFilePointerEx(plain.get(), to, nullptr, FILE_BEGIN) == 0) {
      return false;
    }
    return ::SetEndOfFile(plain.get()) != 0;
  }

  // Sets or clears the sparse attribute on the plain handle. A file
  // system without sparse files refuses, and the writer then runs with
  // the zero-fill it always had.
  [[nodiscard]] bool set_sparse(bool on) noexcept {
    FILE_SET_SPARSE_BUFFER in{};
    in.SetSparse = on ? TRUE : FALSE;
    DWORD got = 0;
    return ::DeviceIoControl(plain.get(), FSCTL_SET_SPARSE, &in,
                             sizeof in, nullptr, 0, &got, nullptr) != 0;
  }

  // Enables SeManageVolumePrivilege on the process token, which is what
  // SetFileValidData checks. False where the account does not hold it,
  // which is every non-elevated session: ERROR_NOT_ALL_ASSIGNED comes
  // back from a successful AdjustTokenPrivileges call in that case.
  [[nodiscard]] static bool enable_manage_volume_privilege() noexcept {
    HANDLE tok = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(),
                           TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                           &tok) == 0) {
      return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok =
        ::LookupPrivilegeValueW(nullptr, L"SeManageVolumePrivilege",
                                &tp.Privileges[0].Luid) != 0 &&
        ::AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) != 0 &&
        ::GetLastError() == ERROR_SUCCESS;
    ::CloseHandle(tok);
    return ok;
  }
};

// The IOCP reader context: one completion port, any number of files,
// reads named by the caller's transfer, completions delivered as callbacks
// out of poll(). The port is the context's; each file associates its
// overlapped handle with it at construction.
//
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is NOT set. With it, a
// read that completes synchronously returns without posting, and its
// callback would have to run inside submit_read() -- exactly what the
// contract forbids. Leaving it unset costs a post per fast completion and
// buys one path: every read, fast or slow, reports from poll().
class iocp_context {
 public:
  class file;

  struct transfer : performed_transfer {
    OVERLAPPED ov{};
    std::byte* dst = nullptr;         // reads fill this
    const std::byte* src = nullptr;   // writes read from this
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool is_write = false;
    // Which file's drain owes this op, and whose handles carry it.
    file* owner_file = nullptr;
    transfer* next_deferred = nullptr;
    bool on_port = false;   // the port owes a completion, so cancel is CancelIoEx
    bool cancelled = false;  // a cancel was asked for; one is enough
  };

  class file {
   public:
    file(iocp_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : ctx_(&ctx) {
      f_.open_file(path, how);
      f_.engage(path.c_str(), how.direct_io, ctx.async_requested_,
                ctx.port_.get());
      name_ = ctx.name_for(f_.direct, f_.use_iocp, how.write);
      name_ += f_.suffix();
      ctx.adopt(*this);
    }

    // Reclaimed, then off the context's list: forget() leaves a file on it,
    // because a file that is merely idled is still open and may be read
    // again. This one is not -- and a destroyed file still on the list is
    // what a later drain() would walk.
    ~file() {
      ctx_->forget(*this);
      ctx_->unlink(*this);
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return f_.size; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    void set_size(std::uint64_t bytes) { f_.set_size(bytes); }

   private:
    friend class iocp_context;

    win_file f_;
    std::string name_ = "readfile";
    iocp_context* ctx_;
    file* next_ = nullptr;      // the context's list of open files
    std::size_t inflight_ = 0;  // this file's share of the context's
  };

  // The port exists whether or not files engage the async path: it is
  // also the wake primitive, so a context whose reads all go through the
  // deferred list still has exactly one thing a blocked poll sleeps on.
  iocp_context(const reader_context_options& opts, unsigned max_inflight)
      : async_requested_(opts.async),
        port_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)),
        queue_depth_(std::max(1u, max_inflight)),
        // Declared last, and initialized last: see the member list.
        pump_(io_lanes::waker<iocp_context>(), this,
              &perform_deferred, io_lanes_for(max_inflight)) {
    if (!port_) {
      throw_winerr("CreateIoCompletionPort");
    }
  }
  iocp_context(const iocp_context&) = delete;
  iocp_context& operator=(const iocp_context&) = delete;

  // Every file is destroyed first, and each one's destructor forgot its
  // own transfers, so the drain finds nothing owed. What can remain is a
  // wake nobody consumed, which the port drops with itself.
  ~iocp_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    arm(op, f, off, buf.size());
    op.dst = buf.data();
    op.src = nullptr;
    op.is_write = false;
    place(op, f);
  }

  // The write half of the same contract, and the same ladder: a length
  // NO_BUFFERING cannot take, or a file whose reopen never engaged, goes
  // to the pump and a lane writes it through the plain handle. Nothing
  // above the context sees the difference.
  //
  // A zero-length write takes the deferred path too: WriteFile of zero
  // bytes posts a completion carrying zero bytes, which is the same
  // shape as a short transfer and cannot be told apart from one.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) noexcept {
    arm(op, f, off, buf.size());
    op.dst = nullptr;
    op.src = buf.data();
    op.is_write = true;
    place(op, f);
  }

 private:
  // What a transfer needs that does not depend on its direction.
  void arm(transfer& op, file& f, std::uint64_t off, std::size_t len) noexcept {
    op.len = len;
    op.off = off;
    op.filled = 0;
    op.next_deferred = nullptr;
    op.owner_file = &f;
    op.on_port = false;
    op.cancelled = false;
    op.reset_performance();
  }

  // How much of a piece of a transfer the port takes; zero means none of
  // it. Asked for both directions and for every piece: the initial one,
  // and what is left after a short completion, which resumes at
  // off + filled -- a granule boundary only if the device stopped on one.
  //
  // A buffered overlapped handle takes any shape, so a buffered file's
  // pieces ride whole. NO_BUFFERING rejects an unaligned length and an
  // unaligned offset, so a direct file's piece is cut to the whole
  // granules it holds at a granule offset. The rest (the sub-granule end
  // of a file, or a resumption off a boundary) goes to a lane, on the
  // plain handle, which is not overlapped and cannot be given to the
  // port. The io_uring backend's piece_for() asks the same question with a
  // plain descriptor that can ride.
  //
  // A zero-length transfer goes to a lane too, because it would post a
  // completion carrying zero bytes, the same shape as a short transfer. A
  // ReadFile or WriteFile refused outright owes no completion, so it also
  // goes to a lane, and a second failure there reaches the caller like any
  // other transfer error.
  [[nodiscard]] DWORD port_len(const transfer& op, std::uint64_t off,
                               std::size_t len) const noexcept {
    const win_file& f = op.owner_file->f_;
    if (!f.use_iocp || len == 0) {
      return 0;
    }
    len = std::min(len, max_win_piece);
    if (f.direct) {
      const std::size_t granules = len - len % direct_align;
      return off % direct_align == 0 ? static_cast<DWORD>(granules) : 0;
    }
    return static_cast<DWORD>(len);
  }

  void place(transfer& op, file& f) noexcept {
    in_flight_++;
    f.inflight_++;
    route(op);
  }

 public:

  // CancelIoEx names one request by its OVERLAPPED, so a sibling read on
  // the same handle is untouched. It fails with ERROR_NOT_FOUND for a
  // request that has already finished, which is not an error here: that
  // completion is already on the port and arrives normally. A transfer
  // the pump has is cancelled through the pump, under its lock, because
  // the flag is what a lane reads when it takes one; one a lane already
  // has runs to completion.
  void cancel(transfer& op) noexcept {
    if (!op.on_port) {
      pump_.cancel(op);
      return;
    }
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    ::CancelIoEx(op.owner_file->f_.h(), &op.ov);
  }

  // ReadFile queues the request itself, so there is nothing batched to
  // push. The contract keeps the call because io_uring has one.
  void flush() noexcept {}

  // The port first, then the pump's finished list: a completion the port
  // hands back can move its remainder to the pump.
  std::size_t poll(bool block) {
    const std::size_t before = reported_;
    reap_ready(false);
    serve_deferred();
    if (reported_ != before || !block) {
      return reported_ - before;
    }
    for (;;) {
      if (std::exchange(woken_, false)) {
        serve_deferred();
        return reported_ - before;
      }
      reap_ready(true);
      serve_deferred();
      if (reported_ != before || std::exchange(woken_, false)) {
        return reported_ - before;
      }
    }
  }

  // Posts a sentinel the port hands back with a null OVERLAPPED, which is
  // how a blocked GetQueuedCompletionStatus is ended from another thread
  // or from a lane reporting a finished transfer.
  void wake() noexcept {
    ::PostQueuedCompletionStatus(port_.get(), 0, wake_key, nullptr);
  }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // The port's depth where any transfer of this run rode it, and one
  // otherwise. A file whose reopen never engaged sends everything to the
  // pump, whose lanes read through one non-overlapped handle. More lanes
  // measured no faster there. On a cached 4 GiB file, --qd 1 and --qd 4
  // take the same wall clock, with the shared handle and with a handle per
  // lane.
  [[nodiscard]] unsigned device_depth() const noexcept {
    return used_port_ ? queue_depth_ : 1u;
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }
  // Transfers, or remainders of transfers, performed by the lanes (or by
  // poll() where no lane runs) and reported since construction: zero for a
  // buffered file on a port, one per direct file whose size is not a
  // multiple of the granule.
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return served_synchronously_;
  }

  // This context's rung and the one it degrades to, declared for the
  // ladder walk in src/io/io_engines.cpp.
  static constexpr io_engine rung_engine = io_engine::iocp;
  using rung_below = win_sync_rung;

  // A constructed iocp_context has a port, because the constructor throws
  // otherwise. This answers the synchronous rung only for a caller who
  // asked for no async, which says nothing about the machine, so that
  // verdict is unsettled.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return async_requested_ ? rung_verdict{rung_engine, true}
                            : rung_verdict{rung_below::rung_engine, false};
  }

  // Cancels what every open file still has on the port, reaps all of it
  // without reporting any, and clears the pump, waiting its lanes out.
  // CancelIoEx bounds the wait: every cancelled request completes,
  // successfully or with ERROR_OPERATION_ABORTED.
  //
  // A null OVERLAPPED that is not the wake sentinel means the port itself
  // has failed. No completion can arrive, and the requests the kernel
  // holds can never be given back. The caller frees the buffers they name
  // as soon as this returns, so the process ends with the reason instead,
  // as the io_uring drain does in the same position.
  void drain() noexcept {
    for (file* f = files_; f != nullptr; f = f->next_) {
      if (f->f_.use_iocp && f->inflight_ > 0) {
        ::CancelIoEx(f->f_.h(), nullptr);
      }
    }
    while (port_owed_ > 0) {
      const port_event e = next(INFINITE);
      if (e.what == port_event::kind::wake) {
        woken_ = true;
        continue;
      }
      if (e.what != port_event::kind::completion) {
        std::fprintf(stderr,
                     "blake3pp: the completion port failed while %zu "
                     "transfer(s) were still owed (error %lu); their buffers "
                     "cannot be released\n",
                     port_owed_, static_cast<unsigned long>(e.err));
        std::abort();
      }
      // Reaped, not reported.
    }
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->inflight_ = 0;
    }
    pump_.clear();
    in_flight_ = 0;
  }

  // Gives up every transfer this file owns, running no callback: what the
  // pump has is dropped or waited out, and what the port has is cancelled
  // and waited out, since a pending request would write into the caller's
  // buffer after it is freed. The file's destructor calls this. Legal only
  // while the operations are alive.
  //
  // The port is shared, so a completion belonging to another file can
  // surface while this one is waited out. It is reported, since dropping
  // it would strand an operation that can never complete, so destroying
  // one file while a sibling has transfers in flight can run that
  // sibling's callback from a destructor. The engines run one file per
  // context; a multi-file caller reclaims a file through
  // io_driver::drain(file&) before closing it.
  void forget(file& f) noexcept {
    // The file stays on files_: it is still open and may be read again (see
    // the uring backend's forget).
    //
    // What never reached the port goes first: it names this file's handles
    // and its share of the count, and the port will never hand it back. A
    // lane inside a transfer on these handles cannot be recalled, so it is
    // waited out, and what it finished is dropped unreported.
    const auto mine = [&f](const transfer& op) noexcept {
      return op.owner_file == &f;
    };
    std::size_t dropped = pump_.remove_queued_if(mine);
    pump_.quiesce();
    dropped += pump_.remove_done_if(mine);
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
    if (!f.f_.use_iocp || f.inflight_ == 0) {
      return;
    }
    ::CancelIoEx(f.f_.h(), nullptr);
    while (f.inflight_ > 0) {
      const port_event e = next(INFINITE);
      if (e.what == port_event::kind::wake) {
        woken_ = true;  // remember it; a later poll() still owes it
        continue;
      }
      if (e.what != port_event::kind::completion) {
        // The port is unusable, so this file's transfers can never come
        // back. Its handles close next and the buffers they are writing into
        // are released. Nothing here can make the kernel let go, and
        // returning would leave the context counting a file that no longer
        // exists, so the process ends with the reason, as the context-wide
        // drain does.
        std::fprintf(stderr,
                     "blake3pp: the completion port failed while %zu "
                     "transfer(s) of a closing file were still owed (error "
                     "%lu); their buffers cannot be released\n",
                     f.inflight_, static_cast<unsigned long>(e.err));
        std::abort();
      }
      transfer& op = *e.op;
      if (op.owner_file == &f) {
        op.device_end();
        in_flight_--;
        f.inflight_--;  // reaped, not reported
        continue;
      }
      (void)settle(op, e.bytes, e.err);
    }
  }

 private:
  static constexpr ULONG_PTR wake_key = ~ULONG_PTR{0};

  // The degradation strings are observable API: tools print them and a
  // run is read differently depending on which rung it names, so the
  // spellings and suffixes are fixed.
  [[nodiscard]] std::string name_for(bool direct, bool use_iocp,
                                     bool write) const {
    const char* const sync_name = write ? "writefile" : "readfile";
    if (use_iocp) {
      return direct ? "iocp+direct" : "iocp";
    }
    return direct ? std::string(sync_name) + "+direct" : std::string(sync_name);
  }

  void adopt(file& f) noexcept {
    f.next_ = files_;
    files_ = &f;
  }

  void unlink(file& f) noexcept {
    for (file** link = &files_; *link != nullptr; link = &(*link)->next_) {
      if (*link == &f) {
        *link = f.next_;
        return;
      }
    }
  }


  // What the transfer still owes, from op.filled: on the port where it
  // will take it, otherwise to a lane. A first piece and the rest of a
  // short one take the same road, as on the ring.
  void route(transfer& op) noexcept {
    const DWORD len = port_len(op, op.off + op.filled, op.len - op.filled);
    op.on_port = len != 0 && issue(op, len);
    used_port_ = used_port_ || op.on_port;
    if (!op.on_port) {
      pump_.push(op);
    }
  }

  // Hands the port `len` bytes of what the transfer still owes, from
  // op.filled: all of it, or the granules of it that port_len() allowed.
  // False when ReadFile or WriteFile refused it outright, which owes no
  // completion; the charge ends there, so a lane's own charge does not
  // take in the wait for the lane.
  [[nodiscard]] bool issue(transfer& op, DWORD len) noexcept {
    std::memset(&op.ov, 0, sizeof(op.ov));
    const std::uint64_t off = op.off + op.filled;
    op.ov.Offset = static_cast<DWORD>(off);
    op.ov.OffsetHigh = static_cast<DWORD>(off >> 32);
#if defined(BLAKE3PP_TEST_IO_SEAMS)
    if (!op.is_write && take_seam(refuse_port_reads)) {
      return false;
    }
    if (!op.is_write && len > 2 * direct_align && take_seam(short_port_reads)) {
      len = static_cast<DWORD>(len / 2 - len / 2 % direct_align);
    }
#endif
    // The piece is the kernel's from here, and charged to the device
    // from here: a cached read completes inside the call.
    op.device_begin();
    const HANDLE h = op.owner_file->f_.h();
    const BOOL ok =
        op.is_write
            ? ::WriteFile(h, op.src + op.filled, len, nullptr, &op.ov)
            : ::ReadFile(h, op.dst + op.filled, len, nullptr, &op.ov);
    if (ok == 0 && ::GetLastError() != ERROR_IO_PENDING) {
      op.device_end();
      return false;
    }
    ++port_owed_;
    return true;
  }

  // Every callback runs from here, which is what poll() counts: a
  // remainder moved to the pump is still owed, and is not one.
  void complete(transfer& op, std::error_code ec) noexcept {
    device_.add_if_served(op.device_total_ns());
    in_flight_--;
    assert(op.owner_file != nullptr && "every transfer is armed with its file");
    op.owner_file->inflight_--;
    ++reported_;
    op.done(&op, ec);
  }

  // The op is the record its OVERLAPPED is a member of. The caller keeps
  // it at a fixed address from submit until its callback has run, which
  // is what makes the walk back valid; it is the documented IOCP idiom
  // and the reason transfer is the caller's type rather than the
  // context's.
  [[nodiscard]] static transfer* op_of(OVERLAPPED* pov) noexcept {
    return CONTAINING_RECORD(pov, transfer, ov);
  }

  // Takes what the port has, running the callbacks of transfers that
  // ended and reissuing the rest of the ones that stopped short. With
  // `block`, waits for the first completion, but not after a remainder
  // the port would not take has moved to the pump: where no lane runs,
  // only poll()'s next step performs it, so a wait on the port with
  // nothing else owed to it would never return.
  void reap_ready(bool block) {
    const std::size_t before = reported_;
    bool parked = false;
    for (;;) {
      const port_event e =
          next(block && reported_ == before && !parked ? INFINITE : 0);
      switch (e.what) {
        case port_event::kind::timeout:
          return;  // nothing more queued
        case port_event::kind::wake:
          woken_ = true;
          return;
        case port_event::kind::failed:
          throw std::system_error(static_cast<int>(e.err),
                                  std::system_category(),
                                  "GetQueuedCompletionStatus");
        case port_event::kind::completion:
          // A rest on the pump ends the wait: see above.
          parked = settle(*e.op, e.bytes, e.err) || parked;
          break;
      }
    }
  }

  // One step of the port: what GetQueuedCompletionStatus handed back. The
  // error is read before anything else runs, since any call -- a clock
  // read included -- may overwrite it, and the key is read only for a
  // dequeue that succeeded with no OVERLAPPED, which is the wake.
  struct port_event {
    enum class kind { completion, wake, timeout, failed };
    kind what;
    transfer* op = nullptr;
    DWORD bytes = 0;
    // A completion's own error, or why the port failed; ERROR_SUCCESS
    // otherwise.
    DWORD err = ERROR_SUCCESS;
  };
  [[nodiscard]] port_event next(DWORD timeout) noexcept {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* pov = nullptr;
    const BOOL ok =
        ::GetQueuedCompletionStatus(port_.get(), &bytes, &key, &pov, timeout);
    const DWORD err = ok != 0 ? ERROR_SUCCESS : ::GetLastError();
    if (pov != nullptr) {
      --port_owed_;
      return {port_event::kind::completion, op_of(pov), bytes, err};
    }
    if (ok == 0 && err == WAIT_TIMEOUT) {
      return {port_event::kind::timeout};
    }
    if (ok != 0 && key == wake_key) {
      return {port_event::kind::wake};
    }
    return {port_event::kind::failed, nullptr, 0, err};
  }

  // What one completion of `op` means: its callback, or the rest of a
  // short transfer reissued. Returns whether that rest went to the pump.
  [[nodiscard]] bool settle(transfer& op, DWORD bytes, DWORD err) noexcept {
    op.device_end();
    if (err != ERROR_SUCCESS) {
      complete(op, read_error(err));
      return false;
    }
    if (bytes == 0) {
      complete(op, std::error_code(EIO, std::generic_category()));
      return false;
    }
    op.filled += bytes;
    if (op.filled < op.len) {
      route(op);
      return !op.on_port;
    }
    complete(op, {});
    return false;
  }

  // One transfer, on a lane. Touches the operation and the file it names,
  // and nothing else of this context, so served_synchronously_ is counted
  // where the completion is run.
  static std::error_code perform_deferred(transfer& op) noexcept {
    // From op.filled, not from zero: a short transfer whose remainder the
    // port would not take arrives here part-done, and the bytes already
    // in the buffer must not be fetched again over themselves.
    try {
      const std::size_t rest = op.len - op.filled;
      if (op.is_write) {
        write_sync(op.owner_file->f_, op.off + op.filled,
                   {op.src + op.filled, rest});
      } else {
        read_sync(op.owner_file->f_, op.off + op.filled,
                  {op.dst + op.filled, rest});
      }
      op.filled = op.len;
    } catch (const std::system_error& e) {
      return e.code();
    } catch (...) {
      // read_sync and write_sync throw only system_error; anything else
      // is reported as EIO so it cannot terminate a noexcept lane.
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  // Runs the callbacks of the transfers the lanes finished, on this
  // thread. The inline step serves a machine that would not give the
  // pump a thread.
  void serve_deferred() noexcept {
    pump_.report_finished(
        [this](transfer& op) noexcept { report_deferred(op); });
  }

  void report_deferred(transfer& op) noexcept {
    const std::error_code ec = op.result();
    if (!ec) {
      served_synchronously_++;
    }
    complete(op, ec);
  }

  // A non-OVERLAPPED handle plus an OVERLAPPED offset blocks until
  // complete. The use_iocp guard makes it structural that the overlapped
  // handle is never used synchronously.
  static void read_sync(win_file& f, std::uint64_t off,
                        std::span<std::byte> buf) {
    const HANDLE use_h =
        f.direct && !f.use_iocp && buf.size() % direct_align == 0
            ? f.h()
            : f.plain.get();
    std::size_t got = 0;
    while (got < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + got;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      const auto ask =
          static_cast<DWORD>(std::min(buf.size() - got, max_win_piece));
      if (::ReadFile(use_h, buf.data() + got, ask, &n, &ov) == 0) {
        throw std::system_error(read_error(::GetLastError()), "ReadFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += n;
    }
  }

  // The write twin of read_sync, and the same handle choice: the
  // overlapped handle is never used synchronously, and a length
  // NO_BUFFERING cannot take goes through the plain one.
  //
  // A zero-length write returns without calling WriteFile, which would
  // only set the file pointer.
  static void write_sync(win_file& f, std::uint64_t off,
                         std::span<const std::byte> buf) {
    if (buf.empty()) {
      return;
    }
    const HANDLE use_h =
        f.direct && !f.use_iocp && buf.size() % direct_align == 0
            ? f.h()
            : f.plain.get();
    std::size_t put = 0;
    while (put < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + put;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      const auto ask =
          static_cast<DWORD>(std::min(buf.size() - put, max_win_piece));
      if (::WriteFile(use_h, buf.data() + put, ask, &n, &ov) == 0) {
        throw_winerr("WriteFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "write made no progress");
      }
      put += n;
    }
  }

  bool async_requested_ = true;
  unique_handle port_;
  file* files_ = nullptr;      // every open file, for the drain
  std::size_t in_flight_ = 0;
  std::size_t served_synchronously_ = 0;
  // Callbacks run since construction; poll() returns its growth.
  std::size_t reported_ = 0;
  std::size_t port_owed_ = 0;  // requests the port has yet to hand back
  // What the device itself took, stamped by the port where it carried a
  // piece and by a lane where one did.
  device_time device_{};
  // What the caller asked to have in flight at once; device_depth()
  // reports it where the port took the transfers.
  unsigned queue_depth_ = 1;
  bool woken_ = false;
  // Whether any transfer of this run reached the port, which is what
  // device_depth() reports against.
  bool used_port_ = false;
  // The lanes that serve what the port will not take. Declared last,
  // so the destructor's join runs while the port its wake() posts to is
  // still open. Do not move this up.
  deferred_pump<transfer> pump_;
};

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time MSVC compiles this
// header, before any engine instantiation exists.
static_assert(io_context<iocp_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
