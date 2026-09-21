// io_driver: the platform's reader context behind a pimpl, plus the
// buffer arena whose lifetime the drain depends on.
//
// Every member forwards to the native context named by
// io/backend_select.hpp. The only real work here is fitting the
// backend's operation inside the caller-owned io_read_op and putting the
// arena on the right side of the drain.

#include <blake3pp/detail/io_driver.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "io/backend.hpp"
#include "io/backend_select.hpp"

namespace blake3pp::detail {

namespace {
using context = io_impl::native_reader_context;
using native_op = context::read_op;

// The backend's operation lives inside the caller's io_read_op. If a
// backend outgrows this, the build stops here rather than writing past
// the storage: raise storage_size in the header.
static_assert(sizeof(native_op) <= io_read_op::storage_size,
              "io_read_op::storage_size is smaller than this platform's "
              "reader_context::read_op");
static_assert(alignof(native_op) <= io_read_op::storage_align,
              "io_read_op::storage_align is weaker than this platform's "
              "reader_context::read_op requires");

// The backend completes its own op; this carries that across to the
// caller's, which is the only one the pipeline knows about.
void trampoline(io_impl::read_op_base* base, std::error_code ec) noexcept {
  auto* const outer = static_cast<io_read_op*>(base->owner);
  outer->done(outer, ec);
}
}  // namespace

// The buffer arena, allocated on demand and freed by its destructor, so
// member order alone decides when that happens relative to the drain.
struct arena {
  std::byte* data = nullptr;
  arena() = default;
  arena(const arena&) = delete;
  arena& operator=(const arena&) = delete;
  ~arena() {
    if (data != nullptr) {
      ::operator delete(data, std::align_val_t{io_impl::direct_align});
    }
  }
};

struct io_driver::impl {
  // Declaration order is the teardown contract: the context drains every
  // read still owed BEFORE the arena those reads target is freed, so the
  // arena is declared FIRST and therefore destroyed LAST. Do not
  // reorder.
  arena buffers;
  context ctx;

  impl(const io_driver_options& opts, unsigned max_inflight)
      : ctx(io_impl::reader_context_options{opts.async, opts.offload_submit},
            max_inflight) {}
};

struct io_driver::file::impl {
  context::file f;
  impl(context& ctx, const std::filesystem::path& path, bool direct_io)
      : f(ctx, path, direct_io) {}
};

io_driver::io_driver(const io_driver_options& opts, unsigned max_inflight)
    : impl_(std::make_unique<impl>(opts, max_inflight)) {}

io_driver::~io_driver() = default;

io_driver::file::file(io_driver& drv, const std::filesystem::path& path,
                      bool direct_io)
    : impl_(std::make_unique<impl>(drv.impl_->ctx, path, direct_io)) {}

io_driver::file::~file() = default;

std::uint64_t io_driver::file::size() const noexcept {
  return impl_->f.size();
}

std::string_view io_driver::file::name() const noexcept {
  return impl_->f.name();
}

void io_driver::submit_read(file& f, std::uint64_t off,
                            std::span<std::byte> buf, io_read_op& op) {
  // The backend's op is constructed in place, once per submit; it holds
  // no state across submissions.
  native_op* const inner = ::new (static_cast<void*>(op.storage)) native_op{};
  inner->done = &trampoline;
  inner->owner = &op;
  impl_->ctx.submit_read(f.impl_->f, off, buf, *inner);
}

void io_driver::flush() { impl_->ctx.flush(); }

std::size_t io_driver::poll(bool block) { return impl_->ctx.poll(block); }

void io_driver::wake() noexcept { impl_->ctx.wake(); }

std::size_t io_driver::in_flight() const noexcept {
  return impl_->ctx.in_flight();
}

std::span<std::byte> io_driver::allocate(std::size_t bytes) {
  // One arena per driver, taken once at pipeline construction. Aligned
  // for direct I/O, which rejects an unaligned buffer outright.
  auto* const raw = static_cast<std::byte*>(
      ::operator new(bytes, std::align_val_t{io_impl::direct_align}));
  impl_->buffers.data = raw;
  return {raw, bytes};
}

}  // namespace blake3pp::detail
