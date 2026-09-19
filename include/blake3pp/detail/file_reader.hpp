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
// buffers are allocated once at construction, and that is the only
// allocation. Windows are delivered strictly in file order, while later
// ones stream in behind them.
//
// release() recycles a buffer, and that is what creates the backpressure:
// at most queue_depth windows are ever in flight or held. Not
// thread-safe, so drive it from one pipeline thread.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

namespace blake3pp::detail {

constexpr unsigned max_queue_depth = 32;

// The largest window or buffer the engines accept, 1 GiB, or less where
// max_queue_depth of them would not fit in size_t. The cap keeps the pool
// size from wrapping and every transfer below the 32-bit length the
// backends hand the kernel (io_uring's sqe.len, WriteFile's DWORD).
constexpr std::size_t max_window_bytes =
    std::min<std::size_t>(std::size_t{1} << 30,
                          std::numeric_limits<std::size_t>::max() / max_queue_depth);

// The window the reader actually uses for a requested size: a power of
// two (so every full window is a subtree-aligned unit) between 64 KiB
// (which keeps O_DIRECT alignment trivial) and max_window_bytes. Anything
// that sizes storage per window must use the same rounding.
[[nodiscard]] constexpr std::size_t rounded_window_bytes(
    std::size_t requested) noexcept {
  return std::bit_floor(
      std::clamp<std::size_t>(requested, 64 * 1024, max_window_bytes));
}

struct file_reader_options {
  // Rounded down to a power-of-2 multiple of the chunk size, min 64 KiB,
  // max 1 GiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;  // clamped to [2, 32]
  bool direct_io = true;     // try O_DIRECT; silently degrade if refused
  bool async = true;         // try io_uring; silently degrade if refused
  // Issue each read on the kernel's worker threads (io_uring: IOSQE_ASYNC)
  // rather than inline in the submit call. Issuing a large direct read is
  // real CPU work (pinning pages, building and queueing the bios) that
  // otherwise lands on the thread that also waits for the hash; see
  // src/io/uring_backend.hpp. Ignored by backends without the notion.
  bool offload_submit = true;
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
