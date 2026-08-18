#pragma once

// The portable boundary over OS-native file reading. One interface, the
// fastest backend the platform and filesystem allow, decided at runtime:
//
//   Linux:  io_uring + O_DIRECT (async, page-cache-bypassing) with graceful
//           per-feature fallback (no O_DIRECT support -> buffered io_uring;
//           no io_uring -> synchronous pread)
//   Windows: IOCP + FILE_FLAG_NO_BUFFERING (async, page-cache-bypassing)
//           with the same per-feature fallback (no port -> sync ReadFile)
//   macOS:  GCD (libdispatch pool) + F_NOCACHE (async, page-cache-
//           bypassing for uncached data; already-cached pages still come
//           from RAM) with the same fallback (no async -> sync pread)
//   POSIX:  synchronous pread
//   other:  buffered stdio
//
// The model: the file is a sequence of fixed-size windows. queue_depth
// buffers are allocated once at construction (the only allocation);
// windows are delivered strictly in file order while later windows stream
// in behind them. release() recycles a buffer, which is what creates
// backpressure: at most queue_depth windows are ever in flight or held.
// Not thread-safe; drive it from one pipeline thread.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace blake3pp::detail {

struct file_reader_options {
  // Rounded down to a power-of-2 multiple of the chunk size, min 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;  // clamped to [2, 32]
  bool direct_io = true;     // try O_DIRECT; silently degrade if refused
  bool async = true;         // try io_uring; silently degrade if refused
};

class file_reader {
 public:
  // Throws std::system_error if the file cannot be opened or statted.
  // std::filesystem::path is the canonical currency: it carries the
  // platform's native encoding, which is what makes the Windows backend
  // implementable without an API break.
  file_reader(const std::filesystem::path& path,
              const file_reader_options& opts);
  ~file_reader();
  file_reader(const file_reader&) = delete;
  file_reader& operator=(const file_reader&) = delete;

  struct window {
    const std::byte* data;
    std::size_t bytes;     // == window_bytes for all but possibly the last
    std::uint64_t offset;  // byte offset within the file
    bool last;             // reaches end of file
    unsigned slot;         // buffer slot; hand back via release()
  };

  [[nodiscard]] std::uint64_t file_size() const noexcept;

  // Next window in file order; blocks until its read completes. Empty at
  // EOF. Throws std::system_error on read failure. The data stays valid
  // until release() of this window (or destruction).
  std::optional<window> next();

  // Recycles the buffer slot, allowing the next pending window's read to
  // be issued into it.
  void release(const window& w) noexcept;

  // Which mechanism was actually engaged, e.g. "io_uring+direct",
  // "iocp+direct", "gcd+nocache", "pread", "readfile", "stdio".
  [[nodiscard]] const char* backend() const noexcept;

 private:
  struct impl;
  impl* impl_;
};

}  // namespace blake3pp::detail
