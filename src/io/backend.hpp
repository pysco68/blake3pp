#pragma once

// The portable contract between the file_reader/file_writer engines and
// the per-OS I/O backends. 
//
// Two contract rules that span every operation, so they live here rather
// than on any one requirement below:
//  - Slot exclusivity: start()/start_write() may only be called for a
//    slot the backend claimed via wants_async(...), and only while
//    nothing else is outstanding on that slot.
//  - Teardown drain: a backend's destructor drains every in-flight
//    operation. The engines declare their buffer pool member BEFORE the
//    backend member precisely so the drain runs before the pool is
//    freed.
// Internal to src/io/, never installed.

#include <cerrno>
#include <string_view>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <span>
#include <system_error>

#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/file_writer.hpp>

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// The O_DIRECT / FILE_FLAG_NO_BUFFERING buffer-and-length granule.
constexpr std::size_t direct_align = 4096;

// The engines' buffer arena: one direct-I/O-aligned allocation, RAII so
// member declaration order alone sequences teardown against the backend.
struct aligned_pool {
  std::byte* data = nullptr;

  explicit aligned_pool(std::size_t bytes)
      : data(static_cast<std::byte*>(
            ::operator new(bytes, std::align_val_t{direct_align}))) {}
  ~aligned_pool() { ::operator delete(data, std::align_val_t{direct_align}); }
  aligned_pool(const aligned_pool&) = delete;
  aligned_pool& operator=(const aligned_pool&) = delete;
};

template <class B>
concept reader_backend =
    // Opens the file and decides (at runtime, per feature) how much of
    // the requested fast path (direct I/O, async engine) it can actually
    // deliver. The unsigned is the engine's queue depth: the most slots
    // that can ever be outstanding at once.
    std::constructible_from<B, const std::filesystem::path&,
                            const file_reader_options&, unsigned> &&
    requires(B b, const B cb, unsigned slot, std::uint64_t off,
             std::span<std::byte> buf) {
      { cb.size() } noexcept -> std::same_as<std::uint64_t>;
      { cb.name() } noexcept -> std::convertible_to<std::string_view>;
      // The whole runtime-degradation ladder folded into one question the
      // engine asks per window: "may THIS (offset, length) ride your
      // async path?" uring/IOCP answer engaged && length aligned
      // (O_DIRECT / NO_BUFFERING reject unaligned lengths), GCD answers
      // engaged (F_NOCACHE has no alignment contract, so the tail rides
      // too), sync backends answer never. The engine doesn't learn why:
      // false just routes the window to read_sync at delivery time.
      // (Current backends ignore the offset, since engine windows start
      // at 64 KiB multiples and it is therefore always granule-aligned,
      // but it is part of the question because O_DIRECT constrains offset
      // alignment too, and a future engine might not guarantee it.)
      { cb.wants_async(off, std::size_t{}) } noexcept -> std::same_as<bool>;
      // Begins an async read of buf at off, owned by `slot`; legal only
      // after wants_async() said yes for exactly this window.
      { b.start(slot, off, buf) };
      // Blocks until `slot`'s read fully completes, reissuing short
      // reads and absorbing OTHER slots' completions when the OS delivers
      // them out of order. Throws std::system_error on failure, including
      // failures a worker thread captured earlier.
      { b.wait(slot) };
      // Positional synchronous read: completes fully or throws. Picks the
      // right handle internally (direct vs buffered) for the length's
      // alignment; the unaligned-tail dance is backend business.
      { b.read_sync(off, buf) };
    };

template <class B>
concept writer_backend =
    // Creates/truncates the file, preallocates if asked (fallocate /
    // SetEndOfFile+VDL / F_PREALLOCATE), and engages what it can of the
    // fast path. The unsigned is the engine's queue depth.
    std::constructible_from<B, const std::filesystem::path&,
                            const file_writer_options&, unsigned> &&
    requires(B b, const B cb, unsigned slot, std::uint64_t off,
             std::span<const std::byte> buf, std::uint64_t written) {
      { cb.name() } noexcept -> std::convertible_to<std::string_view>;
      // The reader's degradation question, minus the offset: the engine
      // writes strictly sequentially and only the final submit may be
      // unaligned, so the length alone decides. False routes the buffer
      // to write_sync.
      { cb.wants_async(std::size_t{}) } noexcept -> std::same_as<bool>;
      // Begins an async write of buf at off, owned by `slot`; legal only
      // after wants_async() said yes for this length. Returns without
      // waiting: the device drains while the producer fills the next
      // buffer.
      { b.start_write(slot, off, buf) };
      // Blocks until `slot` is idle (trivially so on sync backends).
      // This is the engine's backpressure point: acquire() calls it
      // before recycling the slot's buffer. Throws the slot's deferred
      // write error as std::system_error.
      { b.wait_slot(slot) };
      // Positional synchronous write: completes fully or throws. Handle
      // choice (direct vs buffered) for the length's alignment is
      // backend business, same as read_sync.
      { b.write_sync(off, buf) };
      // End of stream: drain every in-flight write, trim the
      // preallocation back to `written` bytes, flush what needs
      // flushing. May be called more than once; the destructor is the
      // error-swallowing fallback for what finish() didn't get to.
      { b.finish(written) };
    };

}  // namespace blake3pp::detail::io_impl
