#pragma once

// The io_context of the synchronous sources, pread and stdio:
// completion_context over an engine with no queue of its own, so every
// transfer goes to the lanes. Internal to src/io/, never installed.

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>

#include "io/completion_context.hpp"
#include "io/ladder.hpp"
#include "io/lanes.hpp"
#include "io/options.hpp"
#include "io/transfer.hpp"
#include "io/waiter.hpp"

namespace blake3pp::detail::io_impl {

// The engine of every synchronous source. place() declines every
// transfer, so the context's pump performs it on a lane, and a blocked
// poll sleeps on the waiter the lanes and wake() signal. The source opens
// a path, knows its size and name, reads and writes a span at an offset,
// and sets the size. pread and stdio differ only there.
//
// A source says whether it can serve two transfers at once through
// Source::positional. pread and pwrite can, because they carry their own
// offset. stdio cannot, because seek-then-read is two calls over one
// cursor. That source gets a single lane, and its transfers run one at a
// time.
template <class Source>
class sync_engine {
  static_assert(std::same_as<decltype(Source::positional), const bool>,
                "a synchronous source must declare whether its transfers "
                "may run concurrently: static constexpr bool positional");

 public:
  struct transfer_part : span_transfer {
    template <class Part>
    void arm_read(Part&, std::uint64_t at, std::span<std::byte> to) noexcept {
      arm_span_read(at, to);
    }
    template <class Part>
    void arm_write(Part&, std::uint64_t at,
                   std::span<const std::byte> from) noexcept {
      arm_span_write(at, from);
    }
  };

  class file_part {
   public:
    file_part(const sync_engine&, const std::filesystem::path& path,
              const file_open_options& how)
        : src_(path, how) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return src_.size(); }
    [[nodiscard]] std::string_view name() const noexcept {
      return src_.name();
    }
    void set_size(std::uint64_t bytes) { src_.set_size(bytes); }
    [[nodiscard]] Source& source() noexcept { return src_; }

   private:
    Source src_;
  };

  // A synchronous context has nothing under it and nothing can refuse it,
  // so its verdict is always settled. The source names the engine, as its
  // name() spells the file's backend string.
  static constexpr io_engine rung_engine = Source::rung_engine;
  using rung_below = void;
  [[nodiscard]] std::string_view mechanism() const noexcept {
    return "poll(wake)";
  }

  sync_engine(const reader_context_options&, unsigned) noexcept {}
  sync_engine(const sync_engine&) = delete;
  sync_engine& operator=(const sync_engine&) = delete;

  // No queue of its own: engaged() is false, and every transfer is the
  // lanes'.
  [[nodiscard]] bool engaged() const noexcept { return false; }
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return {rung_engine, true};
  }
  // The lanes ARE the device's queue here.
  [[nodiscard]] unsigned device_depth(bool, unsigned lanes) const noexcept {
    return lanes;
  }
  // The depth the caller asked for decides how many lanes serve the
  // queue, since more lanes than transfers in flight can never help. A
  // source that cannot take two at once gets one whatever the depth.
  [[nodiscard]] static unsigned lanes_for(unsigned max_inflight) noexcept {
    return Source::positional ? io_lanes_for(max_inflight) : 1u;
  }

  // One transfer, on a lane. Touches the operation and the source of the
  // file it names, and nothing else.
  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    try {
      Source& src = op.owner_file->part().source();
      if (op.is_write) {
        src.write_at(op.position(), op.write_remaining());
      } else {
        src.read_at(op.position(), op.read_remaining());
      }
      op.filled = op.length();
    } catch (const std::system_error& e) {
      return e.code();
    } catch (...) {
      // A source throws only system_error; anything else is reported as
      // EIO so it cannot terminate a noexcept lane.
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  template <class Op>
  [[nodiscard]] bool place(Op&) noexcept {
    return false;
  }
  // place() never takes a transfer, so nothing reaches these.
  template <class Op>
  void cancel(Op&) noexcept {}
  template <class Op, class Refused>
  void flush(Refused&&) noexcept {}
  template <class Op, class Refused>
  void take_back(Refused&&) noexcept {}
  [[nodiscard]] std::error_code take_failure() noexcept { return {}; }

  // The only event is a wake: from another thread, or from a lane that
  // finished a transfer. The flag says whether one is owed and the waiter
  // only rings, so the look poll() takes and the return from a wake cost
  // an atomic exchange, not an eventfd read: those reads sat between a
  // lane's completion and its callback. The doorbell is drained just
  // before a sleep instead, when the poller has nothing else to do.
  //
  // wake() sets the flag, then rings. A wake that lands before the drain
  // is seen by the second look; one that lands after it rings a drained
  // doorbell and ends the sleep. A waiter that cannot sleep is the queue
  // failing.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    for (;;) {
      if (woken_.exchange(false, std::memory_order_acq_rel)) {
        return {event::kind::wake};
      }
      if (!block) {
        return {};
      }
      (void)waiter_.take();
      if (woken_.exchange(false, std::memory_order_acq_rel)) {
        return {event::kind::wake};
      }
      try {
        waiter_.sleep();
      } catch (const std::system_error& e) {
        return {event::kind::failed, nullptr, e.code()};
      } catch (...) {
        return {event::kind::failed, nullptr,
                std::make_error_code(std::errc::io_error)};
      }
    }
  }
  void wake() noexcept {
    woken_.store(true, std::memory_order_release);
    waiter_.wake();
  }

  void cancel_file(file_part&, std::size_t) noexcept {}
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&&) noexcept {
    return 0;
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept { return 0; }
  void begin_drain() noexcept {}
  void end_drain() noexcept {}
  template <class Op>
  void release(Op&) noexcept {}

 private:
  std::atomic<bool> woken_{false};  // a wake next() has not reported
  poll_waiter waiter_;
};

template <class Source>
using sync_context = completion_context<sync_engine<Source>>;

}  // namespace blake3pp::detail::io_impl
