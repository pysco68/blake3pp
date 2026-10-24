// io_driver: the platform's io_context behind a pimpl, plus the buffer
// arena whose lifetime the drain depends on.
//
// Every member forwards to the native context named by
// io/backend_select.hpp. The only real work here is fitting the
// backend's operation inside the caller-owned io_op and putting the
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
using native_op = context::transfer;

// The backend's operation lives inside the caller's io_op, whose
// emplace_native() checks the fit and the trivial destructor where the
// op is constructed. A backend that outgrows the storage fails that
// check: raise storage_size in the header. One whose op acquires
// anything would have to be released in trampoline() before the
// callback runs, which no backend needs today.
static_assert(io_driver::direct_alignment == io_impl::direct_align,
              "the alignment callers plan their reads around must be the "
              "one the backends enforce");

// Takes an operation off the driver's owed list. The list is circular
// around a sentinel the driver owns, so no unlink needs the head.
void unlink_owed(io_op& op) noexcept {
  op.owed_prev->owed_next = op.owed_next;
  op.owed_next->owed_prev = op.owed_prev;
  op.owed_next = op.owed_prev = nullptr;
  op.owed_by = nullptr;
  op.live = false;
}

// The backend completes its own op; this carries that across to the
// caller's, which is the only one the pipeline knows about.
void trampoline(io_impl::transfer_base* base, std::error_code ec) noexcept {
  auto* const outer = static_cast<io_op*>(base->owner);
  // The transfer is settled before the callback runs, so a callback that
  // resubmits this operation -- or a sweep that runs after it -- sees the
  // storage as what it is: nothing the driver owes.
  unlink_owed(*outer);
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
  // Every operation the context owes a callback, as the driver's
  // callers know it: what a drain walks to lower `live` on the ones the
  // context forgot without reporting.
  io_op owed_sentinel{};
#ifndef NDEBUG
  // A file holds a reference to the context that opened it. One that
  // outlives its driver is a use-after-free the backend would only report
  // as a crash somewhere else, so debug builds count them and name the
  // mistake in the driver's destructor.
  unsigned live_files = 0;
#endif

  impl(const io_driver_options& opts, unsigned max_inflight)
      : ctx(io_impl::reader_context_options{
                .async = opts.async,
                .offload_submit = opts.offload_submit,
                .ioring = opts.ioring,
                .registered_buffers = opts.registered_buffers},
            max_inflight) {
    owed_sentinel.owed_next = owed_sentinel.owed_prev = &owed_sentinel;
  }

  void link_owed(io_op& op, const void* by) noexcept {
    op.owed_by = by;
    op.owed_next = owed_sentinel.owed_next;
    op.owed_prev = &owed_sentinel;
    owed_sentinel.owed_next->owed_prev = &op;
    owed_sentinel.owed_next = &op;
  }

  // Drops the list without reading any operation on it, for the
  // teardown paths whose operations may already be gone (a file destroyed
  // with transfers owed, the driver's own destructor). Their `live` stays
  // as it was: nothing here may touch them, which is why those paths are
  // documented as the last resort and the pipelines drain explicitly.
  void abandon_owed() noexcept {
    owed_sentinel.owed_next = owed_sentinel.owed_prev = &owed_sentinel;
  }

  // Lowers `live` on every owed operation, or on those of one file, after
  // the context has forgotten them: afterwards a cancel sweep that meets
  // one of these sees an operation nobody owes. Only for the explicit
  // drains, whose contract keeps the operations alive until they return.
  void forget_owed(const void* by) noexcept {
    io_op* op = owed_sentinel.owed_next;
    while (op != &owed_sentinel) {
      io_op* const next = op->owed_next;
      if (by == nullptr || op->owed_by == by) {
        unlink_owed(*op);
      }
      op = next;
    }
  }
};

struct io_driver::file::impl {
  context::file f;
  io_driver::impl* drv = nullptr;

  impl(io_driver::impl& d, const std::filesystem::path& path,
       const io_impl::file_open_options& how)
      : f(d.ctx, path, how) {
    drv = &d;
#ifndef NDEBUG
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

io_op_layout native_op_layout() noexcept {
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
  // The context's drain, not drain(): the operations a caller left owed
  // at destruction are not promised to exist any more.
  impl_->ctx.drain();
  impl_->abandon_owed();
}

io_driver::file::file(io_driver& drv, const std::filesystem::path& path,
                      const open_options& how)
    : impl_(std::make_unique<impl>(
          *drv.impl_, path,
          io_impl::file_open_options{how.write, how.direct_io,
                                     how.preallocate_bytes,
                                     how.waive_zero_fill})) {}

// A file destroyed with transfers still owed makes the context drain
// itself, which forgets every file's transfers without a callback. The
// operations behind them may already be gone -- that is the reason this
// path exists -- so the list is dropped unread and their `live` is not
// lowered. Every owed operation is one the context has in flight, so an
// empty context with a non-empty list is exactly that case.
io_driver::file::~file() {
  io_driver::impl& d = *impl_->drv;
  impl_.reset();
  if (d.ctx.in_flight() == 0) {
    d.abandon_owed();
  }
}

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
                            std::span<std::byte> buf, io_op& op) {
  native_op& inner = op.emplace_native<native_op>();
  inner.done = &trampoline;
  inner.owner = &op;
  impl_->ctx.submit_read(f.impl_->f, off, buf, inner);
  // Taken: from here the context owes this operation a callback.
  op.live = true;
  impl_->link_owed(op, &f);
}

void io_driver::submit_write(file& f, std::uint64_t off,
                             std::span<const std::byte> buf, io_op& op) {
  native_op& inner = op.emplace_native<native_op>();
  inner.done = &trampoline;
  inner.owner = &op;
  impl_->ctx.submit_write(f.impl_->f, off, buf, inner);
  // Taken: from here the context owes this operation a callback.
  op.live = true;
  impl_->link_owed(op, &f);
}

void io_driver::flush() noexcept { impl_->ctx.flush(); }

std::size_t io_driver::poll(bool block) { return impl_->ctx.poll(block); }

void io_driver::wake() noexcept { impl_->ctx.wake(); }

void io_driver::cancel(io_op& op) noexcept {
  // Only an operation this driver owes can be cancelled. A caller's sweep
  // sees windows, not submits: a window is counted in flight from the
  // moment it is admitted, and its write is submitted only after the fill
  // that feeds it has run. So an op that was never taken, or whose
  // callback has already run, can reach here. Its storage holds no backend
  // operation, or the previous transfer's with a stale ring flag, and
  // must not be handed to the device.
  if (!op.live) {
    return;
  }
  // The backend's own op is the one the context knows, and it lives in
  // the storage this read's submit constructed it in.
  impl_->ctx.cancel(op.native<native_op>());
}

std::size_t io_driver::in_flight() const noexcept {
  return impl_->ctx.in_flight();
}

void io_driver::drain() noexcept {
  impl_->ctx.drain();
  impl_->forget_owed(nullptr);
}

void io_driver::drain(file& f) noexcept {
  // A sibling's completion the wait surfaces is reported through the
  // trampoline and unlinks itself; what is left of this file afterwards
  // is what the context dropped.
  impl_->ctx.forget(f.impl_->f);
  impl_->forget_owed(&f);
}

void io_driver::reserve(unsigned windows, std::size_t window_bytes) {
  // One arena per driver, taken once when its window pool is built.
  // Aligned for direct I/O, which rejects an unaligned buffer outright.
  // A second call would free memory a read still in flight may target,
  // which is why it is a contract violation and not a reallocation.
  assert(!impl_->buffers && "io_driver::reserve() called twice: the arena "
                            "is taken once per driver");
  impl_->window_bytes = window_bytes;
  impl_->arena_bytes = window_bytes * windows;
  impl_->buffers = io_impl::make_aligned_buffer(impl_->arena_bytes);
  // One region, offered to the context before any read names a buffer in
  // it; a context with nothing to do with it ignores the offer.
  io_impl::register_arena_if_supported(
      impl_->ctx, std::span<std::byte>{impl_->buffers.get(),
                                       impl_->arena_bytes});
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
