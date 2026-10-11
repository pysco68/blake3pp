#pragma once

// The write-side mirror of file_reader: sequential file output through the
// fastest mechanism the platform allows, decided at runtime:
//
//   Linux:  io_uring + O_DIRECT (async, page-cache-bypassing) with graceful
//           per-feature fallback (no O_DIRECT -> buffered io_uring;
//           no io_uring -> synchronous pwrite)
//   Windows: IOCP + FILE_FLAG_NO_BUFFERING, preallocation via
//           SetEndOfFile + best-effort SetFileValidData (waives NTFS's
//           synchronous zero-fill to the valid-data length)
//   macOS:  GCD (libdispatch pool) + F_NOCACHE, preallocation via
//           F_PREALLOCATE + ftruncate
//   POSIX:  synchronous pwrite
//   other:  buffered stdio
//
// The model inverts the reader's. acquire() hands out one of queue_depth
// fixed-size buffers, allocated once at construction and the only
// allocation. The caller fills it. submit() queues the write at the next
// sequential offset and returns immediately, so the producer can fill the
// next buffer while the device drains this one. acquire() blocking on a
// still-in-flight slot is the backpressure.
//
// SUBMIT ACCEPTS, FINISH LANDS. submit() never writes: it hands the
// bytes to the I/O context and returns, and they reach the file inside a
// later poll() -- during another acquire(), or during finish(). So a
// write error NEVER comes out of submit(). It surfaces from the next
// acquire() or from finish(), whichever the caller reaches first, and
// the first error is held until one of them reports it, so nothing a
// submit accepted is dropped silently. finish() lands everything
// outstanding before it reports.
//
// O_DIRECT demands 4 KiB-aligned lengths, so only the final submit() may
// be partial or unaligned. That one goes through a plain fd, the same
// trick the reader uses for its tail; the context decides, so this class
// does not know which path a buffer took.
//
// Not thread-safe, so drive it from one producer thread. The buffers it
// hands out may be filled by many.

#include <cstddef>
#include <string_view>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace blake3pp::detail {

// The default geometry for a generated file (write_file(), io.hpp): 64 MiB
// windows, two in flight. Not the reader's budget -- this is blake3ppgen's
// measured shape, from four PCIe 5 drives in RAID 0, 32 threads, 32 GiB
// written: 24.2 GiB/s at 64 MiB x 2 and 23.8 at 64 MiB x 4, against 20.7
// at the uncapped 128 MiB x 4 and 14.8 at 256 MiB x 8. Past a few hundred
// MiB in flight a striped NVMe volume slows down.
inline constexpr std::size_t default_write_window_bytes = 64 * 1024 * 1024;
inline constexpr unsigned default_write_queue_depth = 2;

struct file_writer_options {
  // Rounded up to a multiple of 4 KiB, min 64 KiB, max 1 GiB.
  std::size_t buffer_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;  // clamped to [2, 32]
  bool direct_io = true;     // try O_DIRECT; silently degrade if refused
  bool async = true;         // try io_uring; silently degrade if refused
  // Issue each write on the kernel's worker threads (io_uring: IOSQE_ASYNC)
  // rather than inline in the submit call, the reader's offload_submit for
  // the producer side; see src/io/uring_backend.hpp.
  bool offload_submit = true;
  // Preallocate this many bytes at construction when the total is known.
  // This matters enormously for async direct I/O: writes that EXTEND the
  // file serialize on the inode lock (each waits out journal + allocation),
  // while writes into preallocated extents overlap freely. On Windows the
  // same role is played by SetEndOfFile plus SetFileValidData (privilege
  // permitting). finish() trims the file back to the bytes actually
  // written.
  std::uint64_t preallocate_bytes = 0;
};

class file_writer {
 public:
  // Creates or truncates the file. Throws std::system_error on failure.
  file_writer(const std::filesystem::path& path,
              const file_writer_options& opts);
  ~file_writer();
  file_writer(const file_writer&) = delete;
  file_writer& operator=(const file_writer&) = delete;

  struct buffer {
    std::byte* data;
    std::size_t capacity;  // == buffer_bytes (rounded)
    unsigned slot;
  };

  // Next buffer in round-robin order; blocks until that slot's previous
  // write has landed. Throws std::system_error if any write so far
  // failed.
  buffer acquire();

  // Accepts `bytes` from the buffer for the next sequential file offset
  // and returns without waiting or writing. `bytes` must be a multiple of
  // 4 KiB except on the final submit before finish(). Throws only for a
  // buffer this writer did not hand out, a length larger than it holds,
  // or a submit after a partial one; a WRITE error is reported by the
  // next acquire() or by finish(), never from here.
  void submit(const buffer& b, std::size_t bytes);

  // Lands every accepted byte and surfaces the first write error there
  // was. Implicit in the destructor, but only finish() can report
  // failure, so call it.
  void finish();

  [[nodiscard]] std::uint64_t bytes_written() const noexcept;

  // Which mechanism was actually engaged, e.g. "io_uring+direct",
  // "iocp+direct", "gcd-write+nocache", "pwrite", "writefile", "stdio".
  // A view of the writer's own name: copy it to outlive the writer.
  [[nodiscard]] std::string_view backend() const noexcept;

 private:
  struct impl;
  // unique_ptr over an incomplete type: legal because the destructor is
  // only DECLARED here and defined in the TU where impl is complete. That
  // also keeps this class non-movable by default, which is deliberate:
  // outstanding windows/buffers hold the slot indices this object owns.
  std::unique_ptr<impl> impl_;
};

}  // namespace blake3pp::detail
