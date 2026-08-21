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
  // refused reopen, or a port that will not attach, simply leaves the
  // plain handle in charge with direct/use_iocp still false.
  void engage(const wchar_t* path, DWORD access, DWORD share,
              bool want_direct, bool want_async) noexcept {
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
      unique_handle p(
          ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
      // An unattachable port takes the fast handle down with it: a
      // FILE_FLAG_OVERLAPPED handle cannot serve the synchronous path.
      // Both candidates close on the way out of this branch.
      if (!p ||
          ::CreateIoCompletionPort(cand.get(), p.get(), 0, 0) == nullptr) {
        return;
      }
      port = std::move(p);
      use_iocp = true;
    }
    fast = std::move(cand);
    direct = want_direct;
  }
};

class iocp_reader {
 public:
  iocp_reader(const std::filesystem::path& path,
              const file_reader_options& opts, unsigned nslots)
      : slots_(nslots), ovs_(nslots) {
    file_.open(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
    size_ = file_.stat_size();
    file_.engage(path.c_str(), GENERIC_READ, FILE_SHARE_READ, opts.direct_io,
                 opts.async);
    name_ = file_.use_iocp ? (file_.direct ? "iocp+direct" : "iocp")
            : file_.direct ? "readfile+direct"
                           : "readfile";
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
  ~iocp_reader() {
    if (file_.use_iocp && outstanding_ > 0) {
      drain_cancelled();
    }
  }
  iocp_reader(const iocp_reader&) = delete;
  iocp_reader& operator=(const iocp_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Only fully-aligned windows may ride the IOCP path (NO_BUFFERING
  // rejects unaligned lengths); the tail goes through read_sync.
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t len) const
      noexcept {
    return file_.use_iocp && len % direct_align == 0;
  }

  void start(unsigned s, std::uint64_t off, std::span<std::byte> buf) {
    slots_[s] = {buf.data(), buf.size(), off, 0, false};
    submit_read(s, 0);
  }

  // Reaps completions (issuing continuations for short reads) until slot
  // `s` is fully read. GetQueuedCompletionStatus is wait_one: the
  // OVERLAPPED pointer identifies the slot.
  void wait(unsigned s) {
    while (!slots_[s].ready) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(file_.port.get(), &bytes,
                                                  &key, &pov, INFINITE);
      if (pov == nullptr) {
        throw_winerr("GetQueuedCompletionStatus");
      }
      --outstanding_;
      const unsigned c = static_cast<unsigned>(pov - ovs_.data());
      slot& st = slots_[c];
      if (ok == 0) {
        throw_winerr("iocp read");
      }
      if (bytes == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF (iocp)");
      }
      st.filled += bytes;
      if (st.filled < st.len) {
        submit_read(c, st.filled);
      } else {
        st.ready = true;
      }
    }
  }

  // Positional synchronous read: a non-OVERLAPPED handle plus an
  // OVERLAPPED offset blocks until complete. In iocp mode only unaligned
  // tails reach this path; the !use_iocp guard makes it structural that
  // the overlapped handle is never used synchronously.
  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    const HANDLE use_h = file_.direct && !file_.use_iocp &&
                                 buf.size() % direct_align == 0
                             ? file_.h()
                             : file_.plain.get();
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

 private:
  struct slot {
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool ready = false;
  };

  // Queues one async read (or a short-read continuation from `from`).
  void submit_read(unsigned s, std::size_t from) {
    slot& st = slots_[s];
    OVERLAPPED& ov = ovs_[s];
    std::memset(&ov, 0, sizeof(ov));
    const std::uint64_t off = st.off + from;
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    if (::ReadFile(file_.h(), st.dst + from,
                   static_cast<DWORD>(st.len - from), nullptr, &ov) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
      throw_winerr("ReadFile(async)");
    }
    ++outstanding_;
  }

  std::vector<slot> slots_;
  std::vector<OVERLAPPED> ovs_;  // one per slot; the SQE equivalent
  std::uint64_t size_ = 0;
  unsigned outstanding_ = 0;  // async reads in flight
  std::string_view name_ = "readfile";
  // Declared LAST so it is destroyed FIRST: the handles must close before
  // ovs_ goes away. drain_cancelled() normally guarantees nothing is in
  // flight by then, but it gives up early if the port itself has failed,
  // and closing the handles is what cancels any request still holding an
  // OVERLAPPED in that path.
  win_file file_;  // plain (tail, fallback) + fast reopen + IOCP port
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
static_assert(reader_backend<iocp_reader>);
static_assert(writer_backend<iocp_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
