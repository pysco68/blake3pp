#pragma once

// What a context and a file are opened with, and the direct-I/O-aligned
// buffer arena the engines and the driver read into.
// Internal to src/io/, never installed.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#if defined(__linux__)
#include <sys/mman.h>  // madvise(MADV_HUGEPAGE) for the arena
#endif

#include <blake3pp/detail/driver.hpp>

namespace blake3pp::detail::io_impl {

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

}  // namespace blake3pp::detail::io_impl
