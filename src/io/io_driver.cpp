// io_driver: the platform's io_context behind a pimpl, plus the buffer
// arena whose lifetime the drain depends on.
//
// Every member forwards to the native context named by
// io/backend_select.hpp. The only real work here is fitting the
// backend's operation inside the caller-owned io_read_op and putting the
// arena on the right side of the drain.

#include <blake3pp/detail/io_driver.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "io/backend.hpp"
#include "io/backend_select.hpp"

namespace blake3pp::detail {

namespace {
using context = io_impl::native_io_context;
using native_op = context::read_op;

// The backend's operation lives inside the caller's io_read_op, whose
// emplace_native() checks the fit and the trivial destructor where the
// op is constructed. A backend that outgrows the storage fails that
// check: raise storage_size in the header. One whose op acquires
// anything would have to be released in trampoline() before the
// callback runs, which no backend needs today.
static_assert(io_driver::direct_alignment == io_impl::direct_align,
              "the alignment callers plan their reads around must be the "
              "one the backends enforce");

// The backend completes its own op; this carries that across to the
// caller's, which is the only one the pipeline knows about.
void trampoline(io_impl::read_op_base* base, std::error_code ec) noexcept {
  auto* const outer = static_cast<io_read_op*>(base->owner);
  outer->done(outer, ec);
}
}  // namespace

struct io_driver::impl {
  // Declaration order is the teardown contract: the context drains every
  // read still owed BEFORE the arena those reads target is freed, so the
  // arena is declared FIRST and therefore destroyed LAST. Do not
  // reorder.
  io_impl::aligned_buffer buffers;
  std::size_t window_bytes = 0;
  std::size_t arena_bytes = 0;
  context ctx;
#ifndef NDEBUG
  // A file holds a reference to the context that opened it. One that
  // outlives its driver is a use-after-free the backend would only report
  // as a crash somewhere else, so debug builds count them and name the
  // mistake in the driver's destructor.
  unsigned live_files = 0;
#endif

  impl(const io_driver_options& opts, unsigned max_inflight)
      : ctx(io_impl::reader_context_options{opts.async, opts.offload_submit},
            max_inflight) {}
};

struct io_driver::file::impl {
  context::file f;
#ifndef NDEBUG
  io_driver::impl* drv = nullptr;
#endif

  impl(io_driver::impl& d, const std::filesystem::path& path,
       const io_impl::file_open_options& how)
      : f(d.ctx, path, how) {
#ifndef NDEBUG
    drv = &d;
    ++d.live_files;
#endif
  }

  ~impl() {
#ifndef NDEBUG
    --drv->live_files;
#endif
  }

  impl(const impl&) = delete;
  impl& operator=(const impl&) = delete;
};

io_read_op_layout native_read_op_layout() noexcept {
  return {sizeof(native_op), alignof(native_op)};
}

io_driver::io_driver(const io_driver_options& opts, unsigned max_inflight)
    : impl_(std::make_unique<impl>(opts, max_inflight)) {}

io_driver::~io_driver() {
#ifndef NDEBUG
  assert(impl_->live_files == 0 &&
         "an io_driver::file outlived its io_driver: every file must be "
         "destroyed before the driver it was opened on");
#endif
  drain();
}

io_driver::file::file(io_driver& drv, const std::filesystem::path& path,
                      const open_options& how)
    : impl_(std::make_unique<impl>(
          *drv.impl_, path,
          io_impl::file_open_options{how.write, how.direct_io,
                                     how.preallocate_bytes})) {}

io_driver::file::~file() = default;

std::uint64_t io_driver::file::size() const noexcept {
  return impl_->f.size();
}

std::string_view io_driver::file::name() const noexcept {
  return impl_->f.name();
}

void io_driver::file::set_size(std::uint64_t bytes) {
  impl_->f.set_size(bytes);
}

void io_driver::submit_read(file& f, std::uint64_t off,
                            std::span<std::byte> buf, io_read_op& op) {
  native_op& inner = op.emplace_native<native_op>();
  inner.done = &trampoline;
  inner.owner = &op;
  impl_->ctx.submit_read(f.impl_->f, off, buf, inner);
}

void io_driver::submit_write(file& f, std::uint64_t off,
                             std::span<const std::byte> buf, io_read_op& op) {
  native_op& inner = op.emplace_native<native_op>();
  inner.done = &trampoline;
  inner.owner = &op;
  impl_->ctx.submit_write(f.impl_->f, off, buf, inner);
}

void io_driver::flush() noexcept { impl_->ctx.flush(); }

std::size_t io_driver::poll(bool block) { return impl_->ctx.poll(block); }

void io_driver::wake() noexcept { impl_->ctx.wake(); }

void io_driver::cancel(io_read_op& op) noexcept {
  // The backend's own op is the one the context knows, and it lives in
  // the storage this read's submit constructed it in.
  impl_->ctx.cancel(op.native<native_op>());
}

std::size_t io_driver::in_flight() const noexcept {
  return impl_->ctx.in_flight();
}

void io_driver::drain() noexcept { impl_->ctx.drain(); }

void io_driver::reserve(unsigned windows, std::size_t window_bytes) {
  // One arena per driver, taken once at pipeline construction. Aligned
  // for direct I/O, which rejects an unaligned buffer outright. A second
  // call would free memory a read of the first scope may still target,
  // which is why it is a contract violation and not a reallocation.
  assert(!impl_->buffers && "io_driver::reserve() called twice: the arena "
                            "is taken once per driver");
  impl_->window_bytes = window_bytes;
  impl_->arena_bytes = window_bytes * windows;
  impl_->buffers = io_impl::make_aligned_buffer(impl_->arena_bytes);
}

window_storage io_driver::window_storage_for(file&, unsigned slot,
                                             std::uint64_t,
                                             std::size_t bytes) noexcept {
  const std::size_t at = static_cast<std::size_t>(slot) * impl_->window_bytes;
  assert(at + bytes <= impl_->arena_bytes &&
         "a window asked for arena the reserve did not take");
  const std::span<std::byte> slice{impl_->buffers.get() + at, bytes};
  return {slice, std::span<const std::byte>(slice)};
}

}  // namespace blake3pp::detail
