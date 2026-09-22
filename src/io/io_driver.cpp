// io_driver: the platform's reader context behind a pimpl, plus the
// buffer arena whose lifetime the drain depends on.
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
#include <type_traits>
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

// One io_read_op is reused for read after read -- the window cells hold
// theirs for the pipeline's lifetime -- and each submit constructs a new
// backend op over the previous one's bytes without destroying it. That is
// only sound while destruction is a no-op. A backend whose op acquires
// anything has to be released in trampoline() before the callback runs,
// which no backend needs today.
static_assert(io_driver::direct_alignment == io_impl::direct_align,
              "the alignment callers plan their reads around must be the "
              "one the backends enforce");

static_assert(std::is_trivially_destructible_v<native_op>,
              "this platform's reader_context::read_op has a non-trivial "
              "destructor; io_read_op::storage is reused per read, so the "
              "op must be destroyed in trampoline() before the callback");

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

  impl(io_driver::impl& d, const std::filesystem::path& path, bool direct_io)
      : f(d.ctx, path, direct_io) {
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
}

io_driver::file::file(io_driver& drv, const std::filesystem::path& path,
                      bool direct_io)
    : impl_(std::make_unique<impl>(*drv.impl_, path, direct_io)) {}

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

void io_driver::flush() noexcept { impl_->ctx.flush(); }

std::size_t io_driver::poll(bool block) { return impl_->ctx.poll(block); }

void io_driver::wake() noexcept { impl_->ctx.wake(); }

std::size_t io_driver::in_flight() const noexcept {
  return impl_->ctx.in_flight();
}

std::span<std::byte> io_driver::allocate(std::size_t bytes) {
  // One arena per driver, taken once at pipeline construction. Aligned
  // for direct I/O, which rejects an unaligned buffer outright.
  impl_->buffers = io_impl::make_aligned_buffer(bytes);
  return {impl_->buffers.get(), bytes};
}

}  // namespace blake3pp::detail
