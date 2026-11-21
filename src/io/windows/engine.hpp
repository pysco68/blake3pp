#pragma once

// The Windows engine: the I/O ring where the machine has one, the
// completion port everywhere else, chosen at runtime when the context is
// built. Internal to src/io/, never installed.

#if defined(_WIN32)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "io/completion_context.hpp"
#include "io/io_context.hpp"
#include "io/ladder.hpp"
#include "io/options.hpp"
#include "io/windows/file.hpp"
#include "io/windows/iocp.hpp"
#include "io/windows/ioring.hpp"

namespace blake3pp::detail::io_impl {

// Windows has two completion mechanisms, so it is the one platform whose
// engine is chosen at runtime. An I/O ring is used where the machine has
// one (Windows 11, IORING_VERSION_3) and reader_context_options::ioring
// is on, and the completion port everywhere else. Both are compiled. The
// choice is made once, when the engine is built, and holds for the
// context's lifetime; every engine call forwards to the one chosen, and
// the file's name() says which one ran.
class windows_engine {
 public:
  // The completion port's transfer, OVERLAPPED included, which the ring
  // carries unused: one shape for both, so an operation is one type.
  using transfer_part = iocp_engine::transfer_part;

  // One Windows file: the shared open, then the chosen engine's reopen.
  // A ring that was asked for and not had puts its reason on the port's
  // names, in the suffix convention the io_uring backend uses for a
  // refused ring: "iocp+direct [ioring: no api]".
  class file_part {
   public:
    file_part(const windows_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      f.open_file(path, how);
      if (e.ring_) {
        f.engage(path.c_str(), how.direct_io, true, nullptr);
        name_ = e.ring_->name_for(f.direct, f.overlapped, how.write);
      } else {
        f.engage(path.c_str(), how.direct_io, e.port_->async_requested(),
                 e.port_->port());
        name_ = e.port_->name_for(f.direct, f.overlapped, how.write) +
                e.ring_refusal_;
      }
      name_ += f.suffix();
    }
    [[nodiscard]] std::uint64_t size() const noexcept { return f.size; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    void set_size(std::uint64_t bytes) { f.set_size(bytes); }

    win_file f;

   private:
    std::string name_ = "readfile";
  };

  windows_engine(const reader_context_options& opts, unsigned max_inflight) {
    // A synchronous context goes straight to the port: the ring would
    // create its event only to refuse the ring.
    if (opts.ioring && opts.async) {
      ring_.emplace(opts, max_inflight);
      if (!ring_->engaged()) {
        // Taken before the ring goes: whether the reason it was refused
        // is this Windows (settled) or this moment (not), which is what
        // keeps best_io_engine() from remembering a resource failure.
        ring_refusal_settled_ = ring_->engaged_rung().settled;
        // A ring that could not be had leaves the port as the better of
        // the two fallbacks, since the ring's own ladder would serve every
        // transfer synchronously on the lanes. The reason goes onto the
        // port's file names, where a refused ring would otherwise be
        // invisible.
        ring_refusal_ = no_ioring_suffix(ring_->refusal());
        ring_.reset();
      }
    }
    if (!ring_) {
      port_.emplace(opts, max_inflight);
    }
  }
  windows_engine(const windows_engine&) = delete;
  windows_engine& operator=(const windows_engine&) = delete;

#if defined(BLAKE3PP_HAS_IORING_API)
  // The ring is in this binary, so it is the top of the ladder and the
  // completion port is the rung a refused ring degrades to.
  static constexpr io_engine rung_engine = io_engine::ioring;
  using rung_below = iocp_engine;
#else
  // <ioringapi.h> was not in the SDK this was built against: the ring
  // engine refuses every ring, so the constructor above never keeps one,
  // and the ring is not an engine this binary carries. The ladder is
  // then the port's own, forwarded so the two cannot drift.
  static constexpr io_engine rung_engine = iocp_engine::rung_engine;
  using rung_below = iocp_engine::rung_below;
#endif

  // Which of the two was chosen. A port is not evidence that the ring is
  // permanently out of reach: a CreateIoRing or event failure is a
  // resource error, so the verdict is settled only where the ring's own
  // refusal was.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    if (ring_) {
      return ring_->engaged_rung();
    }
    const rung_verdict port = port_->engaged_rung();
    return {port.engine, port.settled && ring_refusal_settled_};
  }
  [[nodiscard]] bool engaged() const noexcept {
    return ring_ ? ring_->engaged() : port_->engaged();
  }
  [[nodiscard]] std::string_view mechanism() const noexcept {
    return ring_ ? ring_->mechanism() : port_->mechanism();
  }
  [[nodiscard]] unsigned device_depth(bool used, unsigned lanes) const noexcept {
    return ring_ ? ring_->device_depth(used, lanes)
                 : port_->device_depth(used, lanes);
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return win_perform_rest(op);
  }

  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    return ring_ ? ring_->place(op) : port_->place(op);
  }
  template <class Op>
  void cancel(Op& op) noexcept {
    ring_ ? ring_->cancel(op) : port_->cancel(op);
  }
  template <class Op, class Refused>
  void flush(Refused&& refused) noexcept {
    ring_ ? ring_->template flush<Op>(refused)
          : port_->template flush<Op>(refused);
  }
  template <class Op, class Refused>
  void take_back(Refused&& refused) noexcept {
    ring_ ? ring_->template take_back<Op>(refused)
          : port_->template take_back<Op>(refused);
  }
  [[nodiscard]] std::error_code take_failure() noexcept {
    return ring_ ? ring_->take_failure() : port_->take_failure();
  }
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    return ring_ ? ring_->template next<Op>(block)
                 : port_->template next<Op>(block);
  }
  void wake() noexcept { ring_ ? ring_->wake() : port_->wake(); }
  void cancel_file(file_part& p, std::size_t owed) noexcept {
    ring_ ? ring_->cancel_file(p, owed) : port_->cancel_file(p, owed);
  }
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&& mine) noexcept {
    return ring_ ? ring_->template drop_unsubmitted<Op>(mine)
                 : port_->template drop_unsubmitted<Op>(mine);
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept {
    return ring_ ? ring_->kernel_owed() : port_->kernel_owed();
  }
  void begin_drain() noexcept {
    ring_ ? ring_->begin_drain() : port_->begin_drain();
  }
  void end_drain() noexcept { ring_ ? ring_->end_drain() : port_->end_drain(); }
  template <class Op>
  void release(Op& op) noexcept {
    ring_ ? ring_->release(op) : port_->release(op);
  }

 private:
  std::optional<ioring_engine> ring_;
  std::optional<iocp_engine> port_;
  std::string ring_refusal_;  // the suffix the port's files carry, or empty
  // Whether the ring's refusal is a standing fact; false while no ring
  // was asked for, since a caller who declined one says nothing about
  // the machine.
  bool ring_refusal_settled_ = false;
};

using windows_context = completion_context<windows_engine>;
static_assert(io_context<windows_context>);

// A read file is named by the ring, or by the port with the ring's reason
// for refusing appended (file_part above). A read file has no
// preallocation suffix.
static_assert(std::max(ioring_read_name_max,
                       iocp_read_name_max + no_ioring_suffix_max) <=
                  max_read_backend_length,
              "a read backend name would not fit driver_stats::read_backend");

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
