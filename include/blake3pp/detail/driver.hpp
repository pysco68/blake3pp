#pragma once

// The vocabulary a source of windows has to speak, and nothing that
// speaks it.
//
// Two drivers model this: io_driver, the platform's reader behind a
// pimpl, and memory_driver, a span the pipeline reads views out of.
// Neither is named here on purpose -- a caller that only wants the
// in-memory path must not pull in the file I/O layer to get the concept,
// and Phase 7's public context has to host both without preferring one.
//
// Internal to the library. Standard library only, and no execution
// provider: the concept is about reads, not about senders.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace blake3pp::detail {

// What a read must be aligned to for a platform's direct-I/O path to
// take it: both the offset and the length, which is why a file whose
// windows start at an unaligned offset has to be opened buffered rather
// than degrading read by read. The backends check their own constant
// against this one.
inline constexpr std::size_t direct_alignment = 4096;

// A read in flight. Caller-owned and at a fixed address from submit_read
// until its callback has run.
//
// The backend's own operation lives in `storage`: sized and aligned here
// for the widest backend, checked against the real type by a
// static_assert in io_driver.cpp, so a backend that outgrows it fails the
// build rather than corrupting the neighbouring bytes. One io_read_op
// serves read after read, each submit constructing a fresh backend op
// over the last one's bytes, which is why that type has to be trivially
// destructible -- also checked there.
struct io_read_op {
  void (*done)(io_read_op*, std::error_code) noexcept = nullptr;
  void* owner = nullptr;

  static constexpr std::size_t storage_size = 160;
  static constexpr std::size_t storage_align = 16;
  alignas(storage_align) std::byte storage[storage_size];

  // Constructs the backend's operation over the storage, fresh for this
  // submit and over whatever the last one left: the one way a driver
  // fills the storage, so the fit and the trivial destructor are checked
  // wherever one does.
  template <class Op>
  [[nodiscard]] Op& emplace_native() noexcept {
    static_assert(sizeof(Op) <= storage_size,
                  "io_read_op::storage_size is smaller than this backend's op");
    static_assert(alignof(Op) <= storage_align,
                  "io_read_op::storage_align is weaker than this backend's op");
    static_assert(std::is_trivially_destructible_v<Op>,
                  "io_read_op::storage is reused per read, so the op must "
                  "be trivially destructible");
    return *::new (static_cast<void*>(storage)) Op{};
  }

  // The operation emplace_native() left in the storage. Only the driver
  // that submitted this read may ask, and only while the read is owed:
  // the bytes hold that op until the next submit builds another over
  // them. The hop through void* is what keeps this out of
  // reinterpret_cast, as it does in the kernel tables.
  template <class Op>
  [[nodiscard]] Op& native() noexcept {
    return *std::launder(static_cast<Op*>(static_cast<void*>(storage)));
  }
};

// Size and alignment of the platform backend's own read operation: what
// io_read_op::storage has to hold. The static_assert in io_driver.cpp is
// the one that sees the real type, so this reports the margin on
// platforms whose backend nobody here can compile.
struct io_read_op_layout {
  std::size_t size;
  std::size_t align;
};

[[nodiscard]] io_read_op_layout native_read_op_layout() noexcept;

// Where one window's bytes live, as the driver that owns them says.
//
// `read_into` is what a read fills; it is empty for a source whose bytes
// are already in memory, and a read of an empty target is one that has
// nothing to move. `view` is what the pipeline compresses and absorbs,
// and its CONTENT is defined only once the window's read has completed
// -- for an arena the bytes are the driver's until then, and the window
// enforces that rather than trusting its callers.
struct window_storage {
  std::span<std::byte> read_into;
  std::span<const std::byte> view;
};

// What the pipeline needs of whatever is feeding it: a source of
// windows that reports its completions from poll(), on the thread that
// called it.
template <class D>
concept file_driver =
    requires(D& d, const D& cd, typename D::file& f, io_read_op& op,
             std::uint64_t off, std::span<std::byte> buf, bool block,
             std::size_t bytes, unsigned windows) {
      typename D::file;
      { f.size() } noexcept -> std::same_as<std::uint64_t>;
      { f.name() } noexcept -> std::same_as<std::string_view>;
      { d.submit_read(f, off, buf, op) };
      { d.flush() } noexcept;
      { d.poll(block) } -> std::same_as<std::size_t>;
      { d.wake() } noexcept;
      { d.cancel(op) } noexcept;
      { cd.in_flight() } noexcept -> std::same_as<std::size_t>;
      { d.drain() } noexcept;
      // How much window memory the run will use. A driver that owns an
      // arena takes it here; one whose windows are views of memory the
      // caller already has takes nothing and remembers the geometry.
      { d.reserve(windows, bytes) };
      // Where window `slot`, covering `bytes` bytes at `off`, lives.
      { d.window_storage_for(f, windows, off, bytes) }
          -> std::same_as<window_storage>;
    };

}  // namespace blake3pp::detail
