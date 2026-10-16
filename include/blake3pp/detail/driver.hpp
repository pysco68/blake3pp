#pragma once

// The vocabulary a source of windows has to speak, and nothing that
// speaks it.
//
// Two drivers model this: io_driver, the platform's reader behind a
// pimpl, and memory_driver, a span the pipeline reads views out of.
// Neither is named here on purpose -- a caller that only wants the
// in-memory path must not pull in the file I/O layer to get the concept,
// and the async context hosts both without preferring one.
//
// Internal to the library. Standard library only, and no execution
// provider: the concept is about reads, not about senders.

#include <cassert>
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

// Binds an operation's completion to a member of the object that owns it:
//
//   bind_completion<&slot::complete>(slot.op, slot);
//
// The OS hands a backend nothing but an address, so one cast back to the
// owner's type is unavoidable; this is where it lives, instead of a static
// trampoline and a cast at every call site. Works for any operation with
// the `done` and `owner` pair, which is both of them.
//
// A backend's own operation derives from the one the callback is declared
// over, so the callback's parameter type comes from the field rather than
// from the argument: binding a derived op must still produce the base's
// signature.
template <class F>
struct completion_param;
template <class Base>
struct completion_param<void (*)(Base*, std::error_code) noexcept> {
  using type = Base;
};

template <auto Member, class Op, class Owner>
void bind_completion(Op& op, Owner& owner) noexcept {
  using base = typename completion_param<decltype(Op::done)>::type;
  op.owner = &owner;
  op.done = +[](base* b, std::error_code ec) noexcept {
    (static_cast<Owner*>(b->owner)->*Member)(ec);
  };
}

// One transfer in flight, a read or a write. Caller-owned and at a fixed
// address from the submit until its callback has run.
//
// The backend's own operation lives in `storage`: sized and aligned here
// for the widest backend, checked against the real type by a
// static_assert in io_driver.cpp, so a backend that outgrows it fails the
// build rather than corrupting the neighbouring bytes. One io_op serves
// transfer after transfer, each submit constructing a fresh backend op
// over the last one's bytes, which is why that type has to be trivially
// destructible -- also checked there.
struct io_op {
  void (*done)(io_op*, std::error_code) noexcept = nullptr;
  void* owner = nullptr;
  // Whether a driver currently owes this operation a callback, which is
  // also whether `storage` holds a backend operation. Raised by the driver
  // once a submit has been taken and lowered before the callback runs, so
  // a cancel sweep can tell an operation the device has from one that is
  // only admitted: the two are indistinguishable from the window's own
  // state, and cancelling the second reads storage holding no object, or
  // the previous transfer left in it.
  bool live = false;

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
                  "io_op::storage_size is smaller than this backend's op");
    static_assert(alignof(Op) <= storage_align,
                  "io_op::storage_align is weaker than this backend's op");
    static_assert(std::is_trivially_destructible_v<Op>,
                  "io_op::storage is reused per read, so the op must "
                  "be trivially destructible");
    return *::new (static_cast<void*>(storage)) Op{};
  }

  // The operation emplace_native() left in the storage. Only the driver
  // that submitted this read may ask, and only while the read is owed:
  // the bytes hold that op until the next submit builds another over
  // them, and `live` is that window. The hop through void* is what keeps
  // this out of reinterpret_cast, as it does in the kernel tables.
  template <class Op>
  [[nodiscard]] Op& native() noexcept {
    assert(live && "io_op::native() on an operation no driver owes");
    return *std::launder(static_cast<Op*>(static_cast<void*>(storage)));
  }
};

// Size and alignment of the platform backend's own operation: what
// io_op::storage has to hold. The static_assert in io_driver.cpp is
// the one that sees the real type, so this reports the margin on
// platforms whose backend nobody here can compile.
struct io_op_layout {
  std::size_t size;
  std::size_t align;
};

[[nodiscard]] io_op_layout native_op_layout() noexcept;

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
    requires(D& d, const D& cd, typename D::file& f, io_op& op,
             std::uint64_t off, std::span<std::byte> buf, bool block,
             std::size_t bytes, unsigned windows) {
      typename D::file;
      { f.size() } noexcept -> std::same_as<std::uint64_t>;
      { f.name() } noexcept -> std::same_as<std::string_view>;
      // Queues one transfer. A driver that takes it raises op.live and
      // lowers it before the callback runs: that is how a caller's cancel
      // sweep tells a transfer the device has from one only admitted.
      { d.submit_read(f, off, buf, op) };
      { d.flush() } noexcept;
      { d.poll(block) } -> std::same_as<std::size_t>;
      { d.wake() } noexcept;
      { d.cancel(op) } noexcept;
      { cd.in_flight() } noexcept -> std::same_as<std::size_t>;
      { d.drain() } noexcept;
      // The same for one file: afterwards no transfer of this file is
      // referenced and in_flight() counts none of them, so its buffers
      // and the operations naming them may be destroyed. A scope winding
      // down on a driver it shares with others uses this: the whole
      // drain would forget their transfers too.
      { d.drain(f) } noexcept;
      // How much window memory the run will use. A driver that owns an
      // arena takes it here; one whose windows are views of memory the
      // caller already has takes nothing and remembers the geometry.
      { d.reserve(windows, bytes) };
      // Where window `slot`, covering `bytes` bytes at `off`, lives.
      { d.window_storage_for(f, windows, off, bytes) }
          -> std::same_as<window_storage>;
    };

}  // namespace blake3pp::detail
