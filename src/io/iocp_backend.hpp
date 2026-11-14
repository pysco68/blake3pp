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
#include "io/completion_context.hpp"

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
  bool overlapped = false;  // the reopened handle is FILE_FLAG_OVERLAPPED
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
  // and overlapped still false.
  //
  // Per feature. The two flags are asked for together first, and a
  // refusal drops one and asks again: NO_BUFFERING is a property of the
  // filesystem (SMB and some virtual volumes reject it) while OVERLAPPED is
  // not, and on Windows buffered async measures within noise of direct
  // async (docs/performance.md), so the asynchrony is the half to keep.
  //
  // `port`, when given, is the completion port's context's: every file it
  // opens associates its overlapped handle with that one port, which the
  // file does not own. The I/O ring passes none, since its completions
  // come back through the ring.
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
      // The asynchrony first: per the measurement above, Windows can do
      // without the cache bypass more easily than without it.
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
  // async was asked for and there is one, and take the handle only if
  // both steps worked.
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
    if (with_async && port != nullptr &&
        ::CreateIoCompletionPort(cand.get(), port, 0, 0) == nullptr) {
      return false;
    }
    overlapped = with_async;
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

// The synchronous half both Windows contexts' lanes use. The handle is
// the plain one unless the file has a direct handle that is not
// overlapped and the length keeps NO_BUFFERING's contract: an overlapped
// handle is never used synchronously, which is what makes the OVERLAPPED
// offset below a plain positional read. A non-OVERLAPPED handle plus an
// OVERLAPPED offset blocks until complete.
[[nodiscard]] inline HANDLE win_sync_handle(const win_file& f,
                                            std::size_t len) noexcept {
  return f.direct && !f.overlapped && len % direct_align == 0 ? f.h()
                                                              : f.plain.get();
}

inline void win_sync_read(const win_file& f, std::uint64_t off,
                          std::span<std::byte> buf) {
  const HANDLE use_h = win_sync_handle(f, buf.size());
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
      throw std::system_error(EIO, std::generic_category(), "unexpected EOF");
    }
    got += n;
  }
}

// The write twin of win_sync_read, and the same handle choice. A
// zero-length write returns without calling WriteFile, which would only
// set the file pointer.
inline void win_sync_write(const win_file& f, std::uint64_t off,
                           std::span<const std::byte> buf) {
  if (buf.empty()) {
    return;
  }
  const HANDLE use_h = win_sync_handle(f, buf.size());
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

// The part of a transfer both Windows engines carry: what moves, where,
// and how much already has. The completion port's adds the OVERLAPPED
// its requests ride on.
struct win_transfer : performed_transfer {
  std::byte* dst = nullptr;        // reads fill this
  const std::byte* src = nullptr;  // writes read from this
  std::size_t len = 0;
  std::uint64_t off = 0;
  std::size_t filled = 0;
  bool is_write = false;
  bool cancelled = false;  // a cancel was asked for; one is enough

  [[nodiscard]] std::size_t length() const noexcept { return len; }

  template <class Part>
  void arm_read(Part&, std::uint64_t at, std::span<std::byte> to) noexcept {
    arm(at, to.size());
    dst = to.data();
    src = nullptr;
    is_write = false;
  }
  template <class Part>
  void arm_write(Part&, std::uint64_t at,
                 std::span<const std::byte> from) noexcept {
    arm(at, from.size());
    dst = nullptr;
    src = from.data();
    is_write = true;
  }

 private:
  void arm(std::uint64_t at, std::size_t n) noexcept {
    len = n;
    off = at;
    filled = 0;
    cancelled = false;
    reset_performance();
  }
};

// The rest of a Windows transfer, on a lane, from where it stopped: a
// short transfer whose remainder the engine would not take arrives here
// part-done, and the bytes already in the buffer must not be fetched
// again over themselves. Touches the operation and its file and nothing
// else.
template <class Op>
[[nodiscard]] std::error_code win_perform_rest(Op& op) noexcept {
  try {
    const std::size_t rest = op.len - op.filled;
    const win_file& f = op.owner_file->part().f;
    if (op.is_write) {
      win_sync_write(f, op.off + op.filled, {op.src + op.filled, rest});
    } else {
      win_sync_read(f, op.off + op.filled, {op.dst + op.filled, rest});
    }
    op.filled = op.len;
  } catch (const std::system_error& e) {
    return e.code();
  } catch (...) {
    // win_sync_read and win_sync_write throw only system_error; anything
    // else is reported as EIO so it cannot terminate a noexcept lane.
    return std::make_error_code(std::errc::io_error);
  }
  return {};
}

// The completion-port engine under completion_context: one port, any
// number of files, each associating its overlapped handle with it at
// open. The port is also the wake primitive, so a context whose
// transfers all go to the pump still has exactly one thing a blocked
// poll sleeps on.
//
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is NOT set. With it, a read that
// completes synchronously returns without posting, and its callback
// would have to run inside submit_read() -- exactly what the contract
// forbids. Leaving it unset costs a post per fast completion and buys one
// path: every read, fast or slow, reports from poll().
class iocp_engine {
 public:
  struct transfer_part : win_transfer {
    OVERLAPPED ov{};
  };

  class file_part {
   public:
    file_part(const iocp_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      f.open_file(path, how);
      f.engage(path.c_str(), how.direct_io, e.async_requested_, e.port_.get());
      name_ = e.name_for(f.direct, f.overlapped, how.write);
      name_ += f.suffix();
    }
    [[nodiscard]] std::uint64_t size() const noexcept { return f.size; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    void set_size(std::uint64_t bytes) { f.set_size(bytes); }

    win_file f;

   private:
    std::string name_ = "readfile";
  };

  iocp_engine(const reader_context_options& opts, unsigned max_inflight)
      : async_requested_(opts.async),
        port_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)),
        queue_depth_(std::max(1u, max_inflight)) {
    if (!port_) {
      throw_winerr("CreateIoCompletionPort");
    }
  }
  iocp_engine(const iocp_engine&) = delete;
  iocp_engine& operator=(const iocp_engine&) = delete;

  static constexpr io_engine rung_engine = io_engine::iocp;
  // What failed, as the shared context's messages name it.
  static constexpr std::string_view mechanism = "the completion port";
  using rung_below = win_sync_rung;

  // A constructed engine has a port, because the constructor throws
  // otherwise. This answers the synchronous rung only for a caller who
  // asked for no async, which says nothing about the machine, so that
  // verdict is unsettled.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return async_requested_ ? rung_verdict{rung_engine, true}
                            : rung_verdict{rung_below::rung_engine, false};
  }
  [[nodiscard]] bool engaged() const noexcept { return true; }

  // The port's depth where any transfer of this run rode it, and one
  // otherwise. A file whose reopen never engaged sends everything to the
  // pump, whose lanes read through one non-overlapped handle. More lanes
  // measured no faster there.
  [[nodiscard]] unsigned device_depth(bool used, unsigned) const noexcept {
    return used ? queue_depth_ : 1u;
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return win_perform_rest(op);
  }

  // The rest of a transfer, from op.filled, on the port: all of it for a
  // buffered overlapped handle, the whole granules of it at a granule
  // offset for a direct one. The rest (the sub-granule end of a file, or
  // a resumption off a boundary) goes to a lane, on the plain handle,
  // which is not overlapped. A zero-length transfer goes to a lane too,
  // because it would post a completion carrying zero bytes, the same
  // shape as a short transfer. A ReadFile or WriteFile refused outright
  // owes no completion, so it also goes to a lane.
  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    const win_file& f = op.owner_file->part().f;
    std::size_t len = op.len - op.filled;
    if (!f.overlapped || len == 0) {
      return false;
    }
    len = std::min(len, max_win_piece);
    const std::uint64_t at = op.off + op.filled;
    if (f.direct) {
      len -= len % direct_align;
      if (len == 0 || at % direct_align != 0) {
        return false;
      }
    }
    return issue(op, f.h(), at, static_cast<DWORD>(len));
  }

  // CancelIoEx names one request by its OVERLAPPED, so a sibling read on
  // the same handle is untouched. It fails with ERROR_NOT_FOUND for a
  // request that has already finished, which is not an error here: that
  // completion is already on the port and arrives normally.
  template <class Op>
  void cancel(Op& op) noexcept {
    ::CancelIoEx(op.owner_file->part().f.h(), &op.ov);
  }

  // ReadFile queues the request itself, so there is nothing batched to
  // push and nothing to take back.
  template <class Op, class Refused>
  void flush(Refused&&) noexcept {}
  template <class Op, class Refused>
  void take_back(Refused&&) noexcept {}
  void check() const noexcept {}

  // One step of the port. The error is read before anything else runs,
  // since any call -- a clock read included -- may overwrite it, and the
  // key is read only for a dequeue that succeeded with no OVERLAPPED,
  // which is the wake.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* pov = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(port_.get(), &bytes, &key,
                                                &pov, block ? INFINITE : 0);
    const DWORD err = ok != 0 ? ERROR_SUCCESS : ::GetLastError();
    if (pov != nullptr) {
      --port_owed_;
      Op* const op = static_cast<Op*>(CONTAINING_RECORD(pov, transfer_part, ov));
      return {event::kind::completion, op,
              err != ERROR_SUCCESS ? read_error(err) : std::error_code{},
              bytes};
    }
    if (ok == 0 && err == WAIT_TIMEOUT) {
      return {};
    }
    if (ok != 0 && key == wake_key) {
      return {event::kind::wake};
    }
    return {event::kind::failed, nullptr,
            std::error_code(static_cast<int>(err), std::system_category())};
  }

  // The wake is a completion of its own on the port, so next() reports
  // it and there is nothing to take here.
  [[nodiscard]] bool take_wake() noexcept { return false; }
  // Posts a sentinel the port hands back with a null OVERLAPPED, which is
  // how a blocked GetQueuedCompletionStatus is ended from another thread
  // or from a lane reporting a finished transfer.
  void wake() noexcept {
    ::PostQueuedCompletionStatus(port_.get(), 0, wake_key, nullptr);
  }

  // CancelIoEx bounds a closing file's wait: every cancelled request
  // completes, successfully or with ERROR_OPERATION_ABORTED.
  void cancel_file(file_part& p, std::size_t owed) noexcept {
    if (p.f.overlapped && owed > 0) {
      ::CancelIoEx(p.f.h(), nullptr);
    }
  }
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&&) noexcept {
    return 0;
  }
  [[nodiscard]] std::size_t owed() const noexcept { return port_owed_; }

  template <class EachFile>
  void begin_drain(EachFile&& each) noexcept {
    each([this](file_part& p, std::size_t owed) noexcept {
      cancel_file(p, owed);
    });
  }
  void end_drain() noexcept {}

  template <class Op>
  void release(Op&) noexcept {}

  // The degradation strings are observable API: tools print them and a
  // run is read differently depending on which rung it names, so the
  // spellings and suffixes are fixed.
  [[nodiscard]] std::string name_for(bool direct, bool overlapped,
                                     bool write) const {
    const char* const sync_name = write ? "writefile" : "readfile";
    if (overlapped) {
      return direct ? "iocp+direct" : "iocp";
    }
    return direct ? std::string(sync_name) + "+direct" : std::string(sync_name);
  }

 private:
  static constexpr ULONG_PTR wake_key = ~ULONG_PTR{0};

  // Hands the port `len` bytes from `at`. False when ReadFile or
  // WriteFile refused it outright, which owes no completion; the charge
  // ends there, so a lane's own charge does not take in the wait for the
  // lane.
  template <class Op>
  [[nodiscard]] bool issue(Op& op, HANDLE h, std::uint64_t at,
                           DWORD len) noexcept {
    std::memset(&op.ov, 0, sizeof(op.ov));
    op.ov.Offset = static_cast<DWORD>(at);
    op.ov.OffsetHigh = static_cast<DWORD>(at >> 32);
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

  bool async_requested_ = true;
  unique_handle port_;
  std::size_t port_owed_ = 0;  // requests the port has yet to hand back
  unsigned queue_depth_ = 1;
};

using iocp_context = completion_context<iocp_engine>;

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time MSVC compiles this
// header, before any engine instantiation exists.
static_assert(io_context<iocp_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
