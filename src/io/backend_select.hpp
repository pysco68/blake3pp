#pragma once

// The one platform decision left in the I/O layer. Each branch names the
// reader ENGINE the OS gets and the writer backend, and everything else
// (the engines and the backends themselves) is straight-line C++. The
// engine TUs static_assert the concepts (io/backend.hpp) against these
// aliases, so a backend drifting from the contract fails loudly at this
// seam, not somewhere inside the engine.
//
// The reader side names a whole engine, so a platform can bring its own.
// Every platform runs polled_reader_engine over an io_context today, and
// the alias is the seam where an engine could differ. The context is
// named alongside it because detail::io_driver (the file pipeline's
// compiled seam) drives one directly, without an engine over it.
// Internal to src/io/, never installed.

#include "io/engine.hpp"

#if defined(__linux__)
#include "io/uring_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = uring_context;
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
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
    if (opts.ioring) {
      ring_.emplace(opts, max_inflight);
      if (!ring_->using_ring()) {
        // A ring that could not be had leaves the port as the better of
        // the two fallbacks: the ring context's own ladder would serve
        // every transfer synchronously. The reason goes onto the port's
        // file names, where a ring asked for and refused is otherwise
        // invisible; a ring not asked for leaves them as they are.
        if (opts.async) {
          ring_refusal_ = no_ioring_suffix(ring_->refusal());
        }
        ring_.reset();
      }
    }
    if (!ring_) {
      port_.emplace(opts, max_inflight);
    }
  }
  windows_context(const windows_context&) = delete;
  windows_context& operator=(const windows_context&) = delete;

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) {
    if (ring_) {
      ring_->submit_read(*f.ring_, off, buf, arm_ring(op));
    } else {
      port_->submit_read(*f.port_, off, buf, arm_port(op));
    }
  }

  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, transfer& op) {
    if (ring_) {
      ring_->submit_write(*f.ring_, off, buf, arm_ring(op));
    } else {
      port_->submit_write(*f.port_, off, buf, arm_port(op));
    }
  }

  void flush() noexcept {
    if (ring_) {
      ring_->flush();
    } else {
      port_->flush();
    }
  }

  std::size_t poll(bool block) {
    return ring_ ? ring_->poll(block) : port_->poll(block);
  }

  void wake() noexcept {
    if (ring_) {
      ring_->wake();
    } else {
      port_->wake();
    }
  }

  void cancel(transfer& op) noexcept {
    if (ring_) {
      ring_->cancel(op.in.ring);
    } else {
      port_->cancel(op.in.port);
    }
  }

  [[nodiscard]] std::size_t in_flight() const noexcept {
    return ring_ ? ring_->in_flight() : port_->in_flight();
  }

  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return ring_ ? ring_->served_synchronously()
                 : port_->served_synchronously();
  }

  void drain() noexcept {
    if (ring_) {
      ring_->drain();
    } else {
      port_->drain();
    }
  }

  void forget(file& f) noexcept {
    if (ring_) {
      ring_->forget(*f.ring_);
    } else {
      port_->forget(*f.port_);
    }
  }

  // Only the ring has anything to do with the arena; the port's absence
  // of this member is why register_arena_if_supported() exists.
  void register_arena(std::span<std::byte> arena) noexcept {
    if (ring_) {
      ring_->register_arena(arena);
    }
  }

 private:
  // The chosen backend's operation, made the union's active member and
  // given the caller's callback. Every callback in the library reaches its
  // owner through `owner`, never by walking back from a transfer_base, so
  // forwarding done and owner is the whole hand-over. Constructing it at
  // each submit lets one operation serve transfer after transfer, as io_op
  // does with the backend's.
  [[nodiscard]] ioring_context::transfer& arm_ring(transfer& op) noexcept {
    auto& inner = *::new (static_cast<void*>(&op.in.ring))
        ioring_context::transfer{};
    inner.done = op.done;
    inner.owner = op.owner;
    return inner;
  }
  [[nodiscard]] iocp_context::transfer& arm_port(transfer& op) noexcept {
    auto& inner = *::new (static_cast<void*>(&op.in.port))
        iocp_context::transfer{};
    inner.done = op.done;
    inner.owner = op.owner;
    return inner;
  }

  std::optional<ioring_context> ring_;
  std::optional<iocp_context> port_;
  std::string ring_refusal_;  // the suffix the port's files carry, or empty
};

static_assert(io_context<windows_context>);

using native_io_context = windows_context;
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
#elif defined(__APPLE__)
#include "io/gcd_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = gcd_context;
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
#elif defined(__FreeBSD__)
#include "io/aio_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = aio_context;
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
#elif defined(__unix__)
#include "io/pread_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = pread_context;
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
#else
#include "io/stdio_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = stdio_context;
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
#endif
