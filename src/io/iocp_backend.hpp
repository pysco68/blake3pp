#pragma once

// The Windows backend: CreateFileW handles (std::filesystem::path's native
// wide string is the whole reason the public API trades in paths), an I/O
// completion port as the completion queue, and the SeManageVolumePrivilege
// dance for SetFileValidData. The mapping to the io_uring backend is
// nearly 1:1: one OVERLAPPED per buffer slot plays the SQE,
// GetQueuedCompletionStatus plays wait_one, and FILE_FLAG_NO_BUFFERING is
// O_DIRECT (same sector-alignment demands, same buffered-handle escape
// hatch for the unaligned tail). Degrades per-feature at RUNTIME: no port
// -> sync ReadFile/WriteFile, NO_BUFFERING refused -> buffered. Internal
// to src/io/, never installed.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string_view>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_winerr(const char* what) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), what);
}

// Writes that land beyond the file's valid data length force NTFS to
// zero-fill the gap synchronously, the Windows twin of ext4's
// extending-write serialization. SetFileValidData waives the zero-fill,
// but only for callers holding SeManageVolumePrivilege (admins, usually,
// and only if the privilege is enabled in the token). Best-effort by
// design: returns whether it actually took.
inline bool try_set_valid_data(HANDLE file, std::int64_t size) noexcept {
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES,
                         &token) == 0) {
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  bool ok = ::LookupPrivilegeValueW(nullptr, L"SeManageVolumePrivilege",
                                    &tp.Privileges[0].Luid) != 0;
  ok = ok &&
       ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) != 0 &&
       ::GetLastError() == ERROR_SUCCESS;
  ::CloseHandle(token);
  if (!ok) {
    return false;
  }
  LARGE_INTEGER n;
  n.QuadPart = size;
  return ::SetFileValidData(file, n.QuadPart) != 0;
}

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
// handle that always exists, the optional NO_BUFFERING/OVERLAPPED reopen
// next to it (both are per-open flags, hence a second handle), and the
// completion port when async engages. Every handle lives in a
// unique_handle, which makes the type non-copyable by construction and
// unwinds the whole set when a constructor throws part-way through.
//
// `fast` is held ONLY when the reopen genuinely engaged, so it never
// aliases `plain` and no destructor has to test for that.
// Shared verbatim between reader and writer; only access/creation differ.
struct win_file {
  unique_handle plain;  // always-buffered+sync: unaligned tails, fallback
  unique_handle fast;   // the reopened handle, when one engaged
  unique_handle port;   // IOCP, when async engaged
  bool direct = false;
  bool use_iocp = false;

  // The handle the fast path should use: the reopened one when it
  // engaged, else the plain one. Borrowed: the caller never closes it.
  [[nodiscard]] HANDLE h() const noexcept {
    return fast ? fast.get() : plain.get();
  }

  void open(const wchar_t* path, DWORD access, DWORD share, DWORD creation,
            DWORD flags) {
    plain.reset(::CreateFileW(path, access, share, nullptr, creation, flags,
                              nullptr));
    if (!plain) {
      throw_winerr("CreateFileW");
    }
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    LARGE_INTEGER sz;
    if (::GetFileSizeEx(plain.get(), &sz) == 0) {
      throw_winerr("GetFileSizeEx");
    }
    return static_cast<std::uint64_t>(sz.QuadPart);
  }

  // NO_BUFFERING and OVERLAPPED are per-open flags: engage by reopening,
  // keeping the plain handle for unaligned lengths. Best-effort: a
  // refused reopen, or a port that will not attach, leaves the
  // plain handle in charge with direct/use_iocp still false.
  // shared_port, when given, is a port this file does not own: the reader
  // context keeps one port for every file it opens, while the writer still
  // creates its own here. Ownership is the only difference; association
  // and the failure handling are the same either way.
  void engage(const wchar_t* path, DWORD access, DWORD share,
              bool want_direct, bool want_async,
              HANDLE shared_port = nullptr) noexcept {
    if (!want_direct && !want_async) {
      return;
    }
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (want_direct) {
      flags |= FILE_FLAG_NO_BUFFERING;
    }
    if (want_async) {
      flags |= FILE_FLAG_OVERLAPPED;
    }
    unique_handle cand(::CreateFileW(path, access, share, nullptr,
                                     OPEN_EXISTING, flags, nullptr));
    if (!cand) {
      return;
    }
    if (want_async) {
      if (shared_port != nullptr) {
        // An unattachable port takes the fast handle down with it: a
        // FILE_FLAG_OVERLAPPED handle cannot serve the synchronous path,
        // so the candidate closes on the way out.
        if (::CreateIoCompletionPort(cand.get(), shared_port, 0, 0) ==
            nullptr) {
          return;
        }
      } else {
        unique_handle p(
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        if (!p ||
            ::CreateIoCompletionPort(cand.get(), p.get(), 0, 0) == nullptr) {
          return;
        }
        port = std::move(p);
      }
      use_iocp = true;
    }
    fast = std::move(cand);
    direct = want_direct;
  }
};

// The IOCP reader context: one completion port, any number of files,
// reads named by the caller's read_op, completions delivered as callbacks
// out of poll(). The port is the context's; each file associates its
// overlapped handle with it at construction.
//
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is deliberately NOT set. With it, a
// read that completes synchronously returns without posting, and its
// callback would have to run inside submit_read() -- exactly what the
// contract forbids. Leaving it unset costs a post per fast completion and
// buys one path: every read, fast or slow, reports from poll().
class iocp_context {
 public:
  struct read_op : read_op_base {
    OVERLAPPED ov{};
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    win_file* sync_file = nullptr;  // the deferred path needs the handles
    void* owner_file = nullptr;     // which file's drain owes this op
    read_op* next_deferred = nullptr;
  };

  class file {
   public:
    file(iocp_context& ctx, const std::filesystem::path& path,
         bool direct_io) {
      // FILE_SHARE_READ and nothing else: a digest describes the bytes
      // that were there, so nothing may rewrite or shorten the file
      // while it is being read. The cost is that a test cannot truncate
      // a file the pipeline holds open, which is why the truncation
      // case in tests/file_pipeline.cpp does not run here.
      f_.open(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
      size_ = f_.stat_size();
      f_.engage(path.c_str(), GENERIC_READ, FILE_SHARE_READ, direct_io,
                ctx.port_wanted(), ctx.port());
      name_ = f_.use_iocp ? (f_.direct ? "iocp+direct" : "iocp")
              : f_.direct ? "readfile+direct"
                          : "readfile";
      ctx.adopt(*this);
    }

    ~file() {
      if (ctx_ != nullptr) {
        ctx_->forget(*this);
      }
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

   private:
    friend class iocp_context;
    win_file f_;
    std::uint64_t size_ = 0;
    std::string_view name_ = "readfile";
    iocp_context* ctx_ = nullptr;
    file* next_ = nullptr;      // the context's list of open files
    std::size_t inflight_ = 0;  // this file's share of the context's
  };

  // The port exists whether or not files engage the async path: it is
  // also the wake primitive, so a context whose reads all go through the
  // deferred list still has exactly one thing a blocked poll sleeps on.
  iocp_context(const reader_context_options& opts, unsigned)
      : async_requested_(opts.async),
        port_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
    if (!port_) {
      throw_winerr("CreateIoCompletionPort");
    }
  }
  iocp_context(const iocp_context&) = delete;
  iocp_context& operator=(const iocp_context&) = delete;

  // Every file is gone by now (they must outlive nothing and be destroyed
  // first), so their handles are closed and the requests they owned have
  // reported; the drain finds nothing. What can remain is a wake nobody
  // consumed, which the port drops with itself.
  ~iocp_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   read_op& op) {
    op.dst = buf.data();
    op.len = buf.size();
    op.off = off;
    op.filled = 0;
    op.h = f.f_.h();
    op.sync_file = &f.f_;
    op.next_deferred = nullptr;
    op.owner_file = &f;
    // NO_BUFFERING rejects an unaligned length, so only whole granules
    // ride the port; the tail is read synchronously inside poll().
    bool queued = f.f_.use_iocp && buf.size() % direct_align == 0;
    if (queued) {
      try {
        issue(op);
      } catch (const std::system_error&) {
        // ReadFile refused it outright, so the port owes no completion
        // and nothing will ever take this read off in_flight(). It goes
        // down the ladder an unalignable read already takes: read
        // synchronously inside poll(), where a second failure reaches
        // the caller as every other read error does.
        queued = false;
      }
    }
    in_flight_++;
    f.inflight_++;
    if (!queued) {
      deferred_.push(op);
    }
  }

  // ReadFile queues the request itself, so there is nothing batched to
  // push. The contract keeps the call because io_uring has.
  void flush() noexcept {}

  std::size_t poll(bool block) {
    std::size_t ran = reap_ready(false) + run_one_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (std::exchange(woken_, false)) {
        return ran;
      }
      ran += reap_ready(true) + run_one_deferred();
      if (ran > 0 || std::exchange(woken_, false)) {
        return ran;
      }
    }
  }

  // Posts a sentinel the port hands back with a null OVERLAPPED, which is
  // how a blocked GetQueuedCompletionStatus is ended from another thread.
  void wake() noexcept {
    ::PostQueuedCompletionStatus(port_.get(), 0, wake_key, nullptr);
  }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Cancels what every open file still has on the port, reaps all of it
  // without reporting any, and forgets the deferred list. CancelIoEx
  // bounds the wait: every cancelled request completes, successfully or
  // with ERROR_OPERATION_ABORTED. A null OVERLAPPED that is not the wake
  // sentinel means the port itself has failed and nothing more can
  // arrive.
  void drain() noexcept {
    for (file* f = files_; f != nullptr; f = f->next_) {
      if (f->f_.use_iocp && f->inflight_ > 0) {
        ::CancelIoEx(f->f_.h(), nullptr);
      }
    }
    while (port_owed_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      ::GetQueuedCompletionStatus(port_.get(), &bytes, &key, &pov, INFINITE);
      if (pov == nullptr) {
        if (key == wake_key) {
          woken_ = true;
          continue;
        }
        break;
      }
      --port_owed_;
    }
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->inflight_ = 0;
    }
    deferred_.clear();
    in_flight_ = 0;
  }

 private:
  static constexpr ULONG_PTR wake_key = ~ULONG_PTR{0};

  [[nodiscard]] bool port_wanted() const noexcept { return async_requested_; }
  [[nodiscard]] HANDLE port() const noexcept {
    return port_ ? port_.get() : nullptr;
  }

  void adopt(file& f) noexcept {
    f.ctx_ = this;
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

  // A file about to close cancels what it still owes and waits it out: the
  // requests target the engine's pool, which is freed just after, and a
  // request still pending would write into it afterwards. Nothing of this
  // file's is reported, which is the contract's teardown drain.
  //
  // One sharp edge, since the port is shared: a completion belonging to
  // ANOTHER file can surface while this one drains, and it is reported
  // normally rather than dropped -- losing it would strand an op that can
  // never complete. So destroying one file of a multi-file context while
  // a sibling has reads in flight can run that sibling's callback from a
  // destructor. The engine never does it (one file per context), and a
  // multi-file driver should idle a file before closing it.
  void forget(file& f) noexcept {
    unlink(f);
    if (!f.f_.use_iocp || f.inflight_ == 0) {
      return;
    }
    ::CancelIoEx(f.f_.h(), nullptr);
    while (f.inflight_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      const BOOL ok =
          ::GetQueuedCompletionStatus(port_.get(), &bytes, &key, &pov,
                                      INFINITE);
      if (pov == nullptr) {
        if (key == wake_key) {
          woken_ = true;  // remember it; a later poll() still owes it
          continue;
        }
        break;  // the port is unusable; no completion can arrive
      }
      read_op& op = *op_of(pov);
      --port_owed_;
      if (op.owner_file == &f) {
        in_flight_--;
        f.inflight_--;  // reaped, and deliberately not reported
        continue;
      }
      if (ok == 0) {
        complete(op, std::error_code(static_cast<int>(::GetLastError()),
                                     std::system_category()));
      } else if (bytes == 0) {
        complete(op, std::error_code(EIO, std::generic_category()));
      } else {
        op.filled += bytes;
        if (op.filled < op.len) {
          (void)reissue(op);
        } else {
          complete(op, {});
        }
      }
    }
  }

  // Queues the rest of a short read. False means ReadFile refused it and
  // the op was completed with that error instead. Both callers are
  // inside poll(), which is where a read's error is allowed to reach its
  // callback; what cannot happen is leaving it counted and unissued.
  [[nodiscard]] bool reissue(read_op& op) noexcept {
    try {
      issue(op);
      return true;
    } catch (const std::system_error& e) {
      complete(op, e.code());
      return false;
    }
  }

  void issue(read_op& op) {
    std::memset(&op.ov, 0, sizeof(op.ov));
    const std::uint64_t off = op.off + op.filled;
    op.ov.Offset = static_cast<DWORD>(off);
    op.ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    if (::ReadFile(op.h, op.dst + op.filled,
                   static_cast<DWORD>(op.len - op.filled), nullptr,
                   &op.ov) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
      throw_winerr("ReadFile(async)");
    }
    ++port_owed_;
  }

  void complete(read_op& op, std::error_code ec) noexcept {
    in_flight_--;
    if (op.owner_file != nullptr) {
      static_cast<file*>(op.owner_file)->inflight_--;
    }
    op.done(&op, ec);
  }

  // The op is the record its OVERLAPPED is a member of. The caller keeps
  // it at a fixed address from submit until its callback has run, which
  // is what makes the walk back valid; it is the documented IOCP idiom
  // and the reason read_op is the caller's type rather than the
  // context's.
  [[nodiscard]] static read_op* op_of(OVERLAPPED* pov) noexcept {
    return CONTAINING_RECORD(pov, read_op, ov);
  }

  std::size_t reap_ready(bool block) {
    std::size_t ran = 0;
    for (;;) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(
          port_.get(), &bytes, &key, &pov, block && ran == 0 ? INFINITE : 0);
      if (pov == nullptr) {
        if (ok == 0 && ::GetLastError() == WAIT_TIMEOUT) {
          return ran;  // nothing more queued
        }
        if (key == wake_key) {
          woken_ = true;
          return ran;
        }
        throw_winerr("GetQueuedCompletionStatus");
      }
      read_op& op = *op_of(pov);
      --port_owed_;
      if (ok == 0) {
        complete(op, std::error_code(static_cast<int>(::GetLastError()),
                                     std::system_category()));
        ran++;
        continue;
      }
      if (bytes == 0) {
        complete(op, std::error_code(EIO, std::generic_category()));
        ran++;
        continue;
      }
      op.filled += bytes;
      if (op.filled < op.len) {
        if (!reissue(op)) {
          ran++;  // it ended here, with the error ReadFile gave
        }
        continue;
      }
      complete(op, {});
      ran++;
    }
  }

  std::size_t run_one_deferred() noexcept {
    read_op* const next = deferred_.take();
    if (next == nullptr) {
      return 0;
    }
    read_op& op = *next;
    std::error_code ec;
    try {
      read_sync(*op.sync_file, op.off, {op.dst, op.len});
      op.filled = op.len;
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    complete(op, ec);
    return 1;
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
      if (::ReadFile(use_h, buf.data() + got,
                     static_cast<DWORD>(buf.size() - got), &n, &ov) == 0) {
        throw_winerr("ReadFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += n;
    }
  }

  bool async_requested_ = true;
  unique_handle port_;
  deferred_ops<read_op> deferred_;
  file* files_ = nullptr;      // every open file, for the drain
  std::size_t in_flight_ = 0;
  std::size_t port_owed_ = 0;  // requests the port has yet to hand back
  bool woken_ = false;
};

class iocp_writer {
 public:
  iocp_writer(const std::filesystem::path& path,
              const file_writer_options& opts, unsigned nslots)
      : slots_(nslots), ovs_(nslots) {
    // Two opens of one file need explicit sharing on Windows.
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    file_.open(path.c_str(), GENERIC_WRITE, share, CREATE_ALWAYS,
               FILE_ATTRIBUTE_NORMAL);
    bool vdl = false;
    if (opts.preallocate_bytes > 0) {
      // SetEndOfFile is the fallocate twin: writes become overwrites of an
      // existing region. NTFS adds a second lock beyond ext4's, the valid
      // data length: any write landing past VDL zero-fills the gap
      // synchronously. SetFileValidData waives that, privilege permitting.
      LARGE_INTEGER target;
      target.QuadPart = static_cast<std::int64_t>(opts.preallocate_bytes);
      if (::SetFilePointerEx(file_.plain.get(), target, nullptr,
                             FILE_BEGIN) != 0 &&
          ::SetEndOfFile(file_.plain.get()) != 0) {
        prealloc_ = opts.preallocate_bytes;
        vdl = try_set_valid_data(file_.plain.get(), target.QuadPart);
      }
      LARGE_INTEGER zero{};
      ::SetFilePointerEx(file_.plain.get(), zero, nullptr, FILE_BEGIN);
    }
    file_.engage(path.c_str(), GENERIC_WRITE, share, opts.direct_io,
                 opts.async);
    name_ = file_.use_iocp
                ? (file_.direct ? (vdl ? "iocp+direct+vdl" : "iocp+direct")
                                : "iocp")
                : file_.direct
                    ? (vdl ? "writefile+direct+vdl" : "writefile+direct")
                    : "writefile";
  }


  // Cancels every in-flight request and waits for ALL of them to report.
  // The wait is INFINITE on purpose. These requests target the engine's
  // buffer pool, which is declared before this backend and therefore freed
  // AFTER it, so returning while one is still pending hands the kernel a
  // window to write into freed memory. CancelIoEx makes that wait bounded
  // in practice: once it returns, every outstanding request is guaranteed
  // to complete, successfully or with ERROR_OPERATION_ABORTED. A null
  // OVERLAPPED here therefore means the port itself has failed, not that a
  // request is merely slow: no completion can ever arrive, so breaking is
  // the only option left.
  void drain_cancelled() noexcept {
    ::CancelIoEx(file_.h(), nullptr);
    while (outstanding_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      ::GetQueuedCompletionStatus(file_.port.get(), &bytes, &key, &pov,
                                  INFINITE);
      if (pov == nullptr) {
        break;  // port unusable: no completion will ever arrive
      }
      --outstanding_;
    }
  }

  // Only the drain is hand-written now: every handle belongs to file_,
  // whose destructor runs after this body, that is, after the last
  // request has reported, which is the ordering the drain exists for.
  ~iocp_writer() {
    // finish() may have thrown or been skipped, leaving writes in flight.
    if (file_.use_iocp && outstanding_ > 0) {
      drain_cancelled();
    }
  }
  iocp_writer(const iocp_writer&) = delete;
  iocp_writer& operator=(const iocp_writer&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return file_.use_iocp && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    slots_[s] = {buf.data(), buf.size(), off, 0, true};
    submit_async(s, 0);
  }

  void wait_slot(unsigned s) {
    while (slots_[s].busy) {
      reap_one();
    }
  }

  // Positional synchronous write on a non-OVERLAPPED handle; the
  // unaligned tail always takes the buffered handle (NO_BUFFERING rejects
  // unaligned lengths, same story as O_DIRECT).
  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    const HANDLE use_h = file_.direct && !file_.use_iocp &&
                                 buf.size() % direct_align == 0
                             ? file_.h()
                             : file_.plain.get();
    std::size_t put = 0;
    while (put < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + put;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      if (::WriteFile(use_h, buf.data() + put,
                      static_cast<DWORD>(buf.size() - put), &n, &ov) == 0) {
        throw_winerr("WriteFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "WriteFile wrote nothing");
      }
      put += n;
    }
  }

  void finish(std::uint64_t written) {
    for (unsigned s = 0; s < slots_.size(); ++s) {
      wait_slot(s);
    }
    // SetEndOfFile set the size up front; trim back if less was written.
    if (prealloc_ > written) {
      LARGE_INTEGER n;
      n.QuadPart = static_cast<std::int64_t>(written);
      if (::SetFilePointerEx(file_.plain.get(), n, nullptr, FILE_BEGIN) == 0 ||
          ::SetEndOfFile(file_.plain.get()) == 0) {
        throw_winerr("SetEndOfFile(trim)");
      }
    }
    prealloc_ = 0;
  }

 private:
  struct slot {
    const std::byte* src = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t done = 0;
    bool busy = false;
  };

  void submit_async(unsigned s, std::size_t from) {
    slot& st = slots_[s];
    OVERLAPPED& ov = ovs_[s];
    std::memset(&ov, 0, sizeof(ov));
    const std::uint64_t o = st.off + from;
    ov.Offset = static_cast<DWORD>(o);
    ov.OffsetHigh = static_cast<DWORD>(o >> 32);
    if (::WriteFile(file_.h(), st.src + from,
                    static_cast<DWORD>(st.len - from), nullptr, &ov) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
      throw_winerr("WriteFile(async)");
    }
    ++outstanding_;
  }

  void reap_one() {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* pov = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(file_.port.get(), &bytes, &key,
                                                &pov, INFINITE);
    if (pov == nullptr) {
      throw_winerr("GetQueuedCompletionStatus");
    }
    --outstanding_;
    const unsigned s = static_cast<unsigned>(pov - ovs_.data());
    slot& st = slots_[s];
    if (ok == 0 || bytes == 0) {
      throw_winerr("iocp write");
    }
    st.done += bytes;
    if (st.done < st.len) {
      submit_async(s, st.done);
    } else {
      st.busy = false;
    }
  }

  std::vector<slot> slots_;
  std::vector<OVERLAPPED> ovs_;  // one per slot
  std::uint64_t prealloc_ = 0;
  unsigned outstanding_ = 0;
  std::string_view name_ = "writefile";
  // Declared LAST so it is destroyed FIRST: the handles must close before
  // ovs_ goes away. drain_cancelled() normally guarantees nothing is in
  // flight by then, but it gives up early if the port itself has failed,
  // and closing the handles is what cancels any request still holding an
  // OVERLAPPED in that path.
  win_file file_;  // plain (tail, trim) + fast reopen + IOCP port
};

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time MSVC compiles this
// header, before any engine instantiation exists.
static_assert(reader_context<iocp_context>);
static_assert(writer_backend<iocp_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
