#pragma once

// Shared Win32 plumbing for the direct-I/O reader and writer: CreateFileW
// handles (std::filesystem::path's native wide string is the whole reason
// the public API trades in paths), an I/O completion port as the
// completion queue, and the SeManageVolumePrivilege dance for
// SetFileValidData. Internal to src/io/, never installed.
//
// The mapping to the io_uring build is nearly 1:1: one OVERLAPPED per
// buffer slot plays the SQE, GetQueuedCompletionStatus plays wait_one,
// and FILE_FLAG_NO_BUFFERING is O_DIRECT (same sector-alignment demands,
// same buffered-handle escape hatch for the unaligned tail).

#if defined(_WIN32)
#define BLAKE3PP_IO_WIN32 1

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <system_error>

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

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
