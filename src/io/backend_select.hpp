#pragma once

// The one platform decision left in the I/O layer. Each branch names the
// io_context the OS gets, and everything else (the engines and the
// backends themselves) is straight-line C++. The engine TUs
// static_assert the concepts (io/backend.hpp) against these aliases, so a
// backend drifting from the contract fails loudly at this seam, not
// somewhere inside the engine.
//
// Every platform runs polled_reader_engine and polled_writer_engine over
// its context, so the engine aliases are written once, after the
// branches; a platform that brought its own engine would branch there.
// The context is named on its own because detail::io_driver (the file
// pipeline's compiled seam) drives one directly, without an engine over
// it.
// Internal to src/io/, never installed.

#include "io/engine.hpp"

#if defined(__linux__)
#include "io/uring_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = uring_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(_WIN32)
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "io/iocp_backend.hpp"
#include "io/ioring_backend.hpp"
namespace blake3pp::detail::io_impl {

// Windows has two completion mechanisms, so it is the one platform whose
// backend is chosen at runtime. An I/O ring is used where the machine has
// one (Windows 11, IORING_VERSION_3) and reader_context_options::ioring
// is on, and the completion port everywhere else. Both are compiled. The
// choice is one branch at construction, never per operation, and the
// file's name() says which one ran.
class windows_context {
 public:
  class file;

  // One operation for both backends, holding whichever of their two the
  // context chose. A union, because an operation lives inside the
  // caller's io_op, whose storage is sized for one backend's.
  struct transfer : transfer_base {
    union both {
      ioring_context::transfer ring;
      iocp_context::transfer port;
      // Trivially destructible either way, so the union needs no
      // destructor and a submit may construct a fresh one over the last.
      both() noexcept : ring() {}
    } in{};
  };

  class file {
   public:
    file(windows_context& ctx, const std::filesystem::path& path,
         const file_open_options& how) {
      if (ctx.ring_) {
        ring_.emplace(*ctx.ring_, path, how);
      } else {
        port_.emplace(*ctx.port_, path, how);
        // The port's name, and why the ring that was asked for was not
        // had, in the suffix convention the io_uring backend uses for a
        // refused ring: "iocp+direct [ioring: no api]".
        port_name_ = std::string(port_->name()) + ctx.ring_refusal_;
      }
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept {
      return ring_ ? ring_->size() : port_->size();
    }
    [[nodiscard]] std::string_view name() const noexcept {
      return ring_ ? ring_->name() : std::string_view(port_name_);
    }
    void set_size(std::uint64_t bytes) {
      if (ring_) {
        ring_->set_size(bytes);
      } else {
        port_->set_size(bytes);
      }
    }

   private:
    friend class windows_context;
    std::optional<ioring_context::file> ring_;
    std::optional<iocp_context::file> port_;
    std::string port_name_;
  };

  windows_context(const reader_context_options& opts, unsigned max_inflight) {
    // A synchronous context goes straight to the port: the ring context
    // would create its event and lanes only to refuse the ring.
    if (opts.ioring && opts.async) {
      ring_.emplace(opts, max_inflight);
      if (!ring_->using_ring()) {
        // Taken before the ring goes: whether the reason it was refused
        // is this Windows (settled) or this moment (not), which is what
        // keeps best_io_engine() from remembering a resource failure.
        ring_refusal_settled_ = ring_->engaged_rung().settled;
        // A ring that could not be had leaves the port as the better of
        // the two fallbacks, since the ring context's own ladder would
        // serve every transfer synchronously on its lanes. The reason goes
        // onto the port's file names, where a refused ring would otherwise
        // be invisible.
        ring_refusal_ = no_ioring_suffix(ring_->refusal());
        ring_.reset();
      }
    }
    if (!ring_) {
      port_.emplace(opts, max_inflight);
    }
  }
  windows_context(const windows_context&) = delete;
  windows_context& operator=(const windows_context&) = delete;

 private:
  // Calls `fn` with whichever of the two contexts this one chose; `fn`
  // is generic, and part() and arm() pick the half of a file or an
  // operation that belongs to it.
  template <class Fn>
  decltype(auto) with(Fn&& fn) {
    return ring_ ? fn(*ring_) : fn(*port_);
  }
  template <class Fn>
  decltype(auto) with(Fn&& fn) const {
    return ring_ ? fn(*ring_) : fn(*port_);
  }
  static ioring_context::file& part(ioring_context&, file& f) noexcept {
    return *f.ring_;
  }
  static iocp_context::file& part(iocp_context&, file& f) noexcept {
    return *f.port_;
  }
  static ioring_context::transfer& part(ioring_context&,
                                        transfer& op) noexcept {
    return op.in.ring;
  }
  static iocp_context::transfer& part(iocp_context&, transfer& op) noexcept {
    return op.in.port;
  }

  // The chosen backend's operation, made the union's active member and
  // given the caller's callback. Every callback in the library reaches its
  // owner through `owner`, never by walking back from a transfer_base, so
  // forwarding done and owner is the whole hand-over. Constructing it at
  // each submit lets one operation serve transfer after transfer, as io_op
  // does with the backend's.
  [[nodiscard]] static ioring_context::transfer& arm(ioring_context&,
                                                     transfer& op) noexcept {
    auto& inner = *::new (static_cast<void*>(&op.in.ring))
        ioring_context::transfer{};
    inner.done = op.done;
    inner.owner = op.owner;
    return inner;
  }
  [[nodiscard]] static iocp_context::transfer& arm(iocp_context&,
                                                   transfer& op) noexcept {
    auto& inner = *::new (static_cast<void*>(&op.in.port))
        iocp_context::transfer{};
    inner.done = op.done;
    inner.owner = op.owner;
    return inner;
  }

 public:
  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    with([&](auto& c) noexcept {
      c.submit_read(part(c, f), off, buf, arm(c, op));
    });
  }
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, transfer& op) noexcept {
    with([&](auto& c) noexcept {
      c.submit_write(part(c, f), off, buf, arm(c, op));
    });
  }
  void flush() noexcept {
    with([](auto& c) noexcept { c.flush(); });
  }
  std::size_t poll(bool block) {
    return with([&](auto& c) { return c.poll(block); });
  }
  void wake() noexcept {
    with([](auto& c) noexcept { c.wake(); });
  }
  void cancel(transfer& op) noexcept {
    with([&](auto& c) noexcept { c.cancel(part(c, op)); });
  }
  [[nodiscard]] unsigned device_depth() const noexcept {
    return with([](const auto& c) noexcept { return c.device_depth(); });
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return with([](const auto& c) noexcept { return c.lane_wait_ns(); });
  }
  [[nodiscard]] device_time device() const noexcept {
    return with([](const auto& c) noexcept { return c.device(); });
  }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return with([](const auto& c) noexcept { return c.in_flight(); });
  }
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return with(
        [](const auto& c) noexcept { return c.served_synchronously(); });
  }
  void drain() noexcept {
    with([](auto& c) noexcept { c.drain(); });
  }
  void forget(file& f) noexcept {
    with([&](auto& c) noexcept { c.forget(part(c, f)); });
  }

#if defined(BLAKE3PP_HAS_IORING_API)
  // The ring is in this binary, so it is the top of the ladder and the
  // completion port is the rung a refused ring degrades to -- which is
  // the composition two members below, declared.
  static constexpr io_engine rung_engine = io_engine::ioring;
  using rung_below = iocp_context;
#else
  // <ioringapi.h> was not in the SDK this was built against:
  // ioring_context compiles to a context that refuses every ring, the
  // constructor below therefore never keeps one, and the ring is not an
  // engine this binary carries. The ladder is then the port's own,
  // forwarded so the two cannot drift, and a build without the SDK
  // header does not report an engine it lacks.
  static constexpr io_engine rung_engine = iocp_context::rung_engine;
  using rung_below = iocp_context::rung_below;
#endif

  // Which of the two this context chose, decided once at construction --
  // the same answer file::name() gives below, from the same two members
  // and in the same shape.
  //
  // A port is not evidence that the ring is permanently out of reach: a
  // CreateIoRing or event failure is a resource error, so the verdict is
  // settled only where the ring's own refusal was.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    if (ring_) {
      return ring_->engaged_rung();
    }
    const rung_verdict port = port_->engaged_rung();
    return {port.engine, port.settled && ring_refusal_settled_};
  }

 private:
  std::optional<ioring_context> ring_;
  std::optional<iocp_context> port_;
  std::string ring_refusal_;  // the suffix the port's files carry, or empty
  // Whether the ring's refusal is a standing fact; false while no ring
  // was asked for, since a caller who declined one says nothing about
  // the machine.
  bool ring_refusal_settled_ = false;
};

static_assert(io_context<windows_context>);

using native_io_context = windows_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(__APPLE__)
#include "io/gcd_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = gcd_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(__FreeBSD__)
#include "io/aio_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = aio_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(__unix__)
#include "io/pread_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = pread_context;
}  // namespace blake3pp::detail::io_impl
#else
#include "io/stdio_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = stdio_context;
}  // namespace blake3pp::detail::io_impl
#endif

namespace blake3pp::detail::io_impl {
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
