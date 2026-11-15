#pragma once

// A source whose windows are already in memory.
//
// The pipeline was written for a device: a window is a buffer the driver
// fills and hands back from poll(). A span needs none of that, and the
// cheapest way to say so is a driver whose windows are VIEWS of the
// caller's bytes -- nothing is allocated, nothing is copied, and a read
// is the act of noticing that the bytes are already there.
//
// What it still does is complete from poll(), on the thread that called
// it. That is not ceremony: the scope starts the next window from inside
// a completion, so a read that completed inside submit_read() would run
// the rest of one window's chain inside the start of another, which is
// the recursion the pipeline's second rule exists to prevent. The queue
// below is one round's worth of noticing, deferred to the one place
// callbacks are allowed to run.
//
// Internal to the library. Standard library only; it names no execution
// provider, and the driver concept it models names no driver.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <blake3pp/detail/contract.hpp>
#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/queued_reads.hpp>

namespace blake3pp::detail {

// A source whose windows are views of bytes the caller already has:
// nothing is allocated, nothing is copied, and a read is the act of
// noticing that the bytes are there.
class memory_driver {
 public:
  // The bytes of one "file". Several may share a driver, which is what
  // a caller hashing a sequence of spans through one scope needs.
  class file {
   public:
    file(memory_driver&, std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept {
      return static_cast<std::uint64_t>(bytes_.size());
    }
    [[nodiscard]] std::string_view name() const noexcept { return "memory"; }

   private:
    friend class memory_driver;
    std::span<const std::byte> bytes_;
  };

  memory_driver() = default;
  memory_driver(const memory_driver&) = delete;
  memory_driver& operator=(const memory_driver&) = delete;

  // Nothing is owned, so there is nothing to size. The geometry is kept
  // only to check that windows ask for slots the plan accounted for;
  // kept in every build, so the type has one layout whatever NDEBUG a
  // translation unit was compiled with.
  void reserve(unsigned windows,
               [[maybe_unused]] std::size_t window_bytes) noexcept {
    windows_ = windows;
  }

  // A window of a span is a subspan of it. Nothing to read into, because
  // the bytes never move.
  // @throws std::logic_error for a slot or a window the geometry does not
  //         cover: past the end this hands back a view of the caller's own
  //         memory beyond the span being hashed.
  [[nodiscard]] window_storage window_storage_for(
      file& f, unsigned slot, std::uint64_t off, std::size_t bytes) {
    // Checked in every build: the subspan below is a view into the
    // caller's own memory, and a window past its end is a read of whatever
    // follows it -- which on this driver is the caller's address space
    // rather than an arena of ours.
    require(slot < windows_,
            "window_storage_for(): a slot outside the reserved geometry");
    require(off + bytes <= f.bytes_.size(),
            "window_storage_for(): a window past the end of the span being "
            "hashed");
    return {{}, f.bytes_.subspan(static_cast<std::size_t>(off), bytes)};
  }

  void submit_read(file& f, std::uint64_t, std::span<std::byte>,
                   io_op& op) noexcept {
    reads_.push(op, &f);
  }

  void flush() noexcept {}
  std::size_t poll(bool block) { return reads_.poll(block); }
  void wake() noexcept { reads_.wake(); }
  void cancel(io_op& op) noexcept { reads_.cancel(op); }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return reads_.in_flight();
  }
  void drain() noexcept { reads_.drain(); }
  // One file's share of it: this file's queued reads are forgotten and
  // every other file's stay, since several spans may share one driver and
  // a sibling whose read was dropped would never complete its chain.
  void drain(file& f) noexcept { reads_.drain(&f); }

 private:
  queued_reads reads_;
  unsigned windows_ = 0;
};

static_assert(file_driver<memory_driver>);

}  // namespace blake3pp::detail
