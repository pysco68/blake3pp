#pragma once

// The operation a transfer is named by: transfer_base, the part the
// driver and the callbacks see; performed_transfer, what a mechanism
// records while it performs one; span_transfer, the buffers, offset and
// progress the completion engines and their lanes share.
// Internal to src/io/, never installed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

#include <blake3pp/detail/driver.hpp>

namespace blake3pp::detail::io_impl {

// One transfer in flight, a read or a write. The caller owns it and keeps
// it at a fixed address from the submit until its callback has run;
// backends derive their own op type from this and recover it from whatever
// the OS hands back (io_uring's user_data, IOCP's OVERLAPPED*). `done` is
// called exactly once per submitted transfer, with an empty error_code on
// success.
struct transfer_base {
  void (*done)(transfer_base*, std::error_code) noexcept = nullptr;
  void* owner = nullptr;  // for the caller's use
};

// device_time, the one the drivers report, is declared in
// detail/driver.hpp because it crosses the pimpl. It is stamped by
// whoever performs a transfer, the only place the device's own time can
// be told apart from a span of the pipeline's timeline: a lane stamps
// around its own pread, which excludes its wait for a lane, and an
// asynchronous engine stamps from submission to completion.


// What a transfer needs while a mechanism is performing it. Every context
// that performs transfers derives its transfer from this; transfer_base
// is the part the driver and the callbacks see.
struct performed_transfer : transfer_base {
  // The device's own time over this transfer, summed over the pieces it
  // was carried in, and the error the performer saw.
  //
  // One accumulator and no start stamp: beginning a piece subtracts the
  // clock and ending it adds the clock back, so what is left is the sum
  // of the stretches. Eight bytes instead of sixteen keeps the Windows
  // union inside io_op's storage.
  void device_begin() noexcept {
    if (!charging) {
      charging = true;
      device_ns -= steady_now_ns();
    }
  }
  // Ends the charge, whether the piece landed or was taken back unsent.
  // Idempotent, so a backend may call it wherever a piece leaves the
  // mechanism's hands without tracking which of those places it was.
  void device_end() noexcept {
    if (charging) {
      charging = false;
      device_ns += steady_now_ns();
    }
  }
  // What the device had this transfer for, for device_time::add(). Valid
  // once no piece is outstanding, which is what reporting a transfer
  // means.
  [[nodiscard]] std::uint64_t device_total_ns() const noexcept {
    return device_ns > 0 ? static_cast<std::uint64_t>(device_ns) : 0;
  }

  // The performer's result, as four bytes and a flag instead of the
  // sixteen a std::error_code takes. Two categories reach here: errno,
  // from every POSIX source and every cancel, and the Windows system
  // category. A code from any other category keeps its value and reads
  // back in the generic category.
  void set_result(std::error_code ec) noexcept {
    result_value = ec.value();
    result_is_system = ec.category() == std::system_category();
  }
  [[nodiscard]] std::error_code result() const noexcept {
    if (result_value == 0) {
      return {};
    }
    return result_is_system
               ? std::error_code(result_value, std::system_category())
               : std::error_code(result_value, std::generic_category());
  }

  // Back to the state a fresh submit starts from. Called where a backend
  // arms a transfer, since one operation serves transfer after transfer.
  void reset_performance() noexcept {
    device_ns = 0;
    result_value = 0;
    charging = false;
    result_is_system = false;
  }

  std::int64_t device_ns = 0;
  std::int32_t result_value = 0;
  bool charging = false;          // a piece is outstanding
  bool result_is_system = false;  // the category of result_value
};

// What a transfer moves, where, and how far it has got, in the one
// vocabulary the completion engines and their lanes share: the caller's
// buffer as a span for each direction, the file offset it starts at, and
// what has moved so far. Direction is its own flag, since an empty span
// cannot say whether a zero-length transfer was a read or a write. Each
// engine's transfer derives from this and arms it through its own
// arm_read()/arm_write(), which reset its own state as well.
struct span_transfer : performed_transfer {
  std::span<std::byte> buf{};         // reads fill this
  std::span<const std::byte> wbuf{};  // writes read from this
  std::uint64_t off = 0;
  std::size_t filled = 0;
  bool is_write = false;
  bool cancelled = false;  // a cancel was asked for; one is enough

  [[nodiscard]] std::size_t length() const noexcept {
    return is_write ? wbuf.size() : buf.size();
  }
  // Where the rest of the transfer starts in the file, and how much of it
  // there is: a fresh transfer's whole length from its offset, or what a
  // short piece left.
  [[nodiscard]] std::uint64_t position() const noexcept { return off + filled; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return length() - filled;
  }
  // The caller's bytes the rest moves, in the transfer's direction.
  [[nodiscard]] std::span<std::byte> read_remaining() const noexcept {
    return buf.subspan(filled);
  }
  [[nodiscard]] std::span<const std::byte> write_remaining() const noexcept {
    return wbuf.subspan(filled);
  }

 protected:
  void arm_span_read(std::uint64_t at, std::span<std::byte> to) noexcept {
    arm_span(at);
    buf = to;
    wbuf = {};
    is_write = false;
  }
  void arm_span_write(std::uint64_t at,
                      std::span<const std::byte> from) noexcept {
    arm_span(at);
    buf = {};
    wbuf = from;
    is_write = true;
  }

 private:
  void arm_span(std::uint64_t at) noexcept {
    off = at;
    filled = 0;
    cancelled = false;
    reset_performance();
  }
};

}  // namespace blake3pp::detail::io_impl
