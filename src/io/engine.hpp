#pragma once

// The two portable I/O engines, written exactly once as class templates
// constrained by the backend concepts (io/backend.hpp). The public
// file_reader/file_writer TUs instantiate them with the platform backend
// backend_select.hpp picks; the tests instantiate them again with the
// off-platform POSIX/stdio backends, so those stay compiled AND executed
// on every platform even though the selector never chooses them there.
// The constraint is the contract: an engine can only speak the concept's
// vocabulary, and a backend drifting from it fails at the instantiation
// with a diagnostic naming the missed requirement. Internal to src/io/,
// never installed.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/file_writer.hpp>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

// The read-side window/slot engine: windows are delivered strictly in
// file order while later windows stream in behind them; release()
// recycles a buffer slot, which is what creates backpressure. Every
// window is just "async in flight" (wait) or "read lazily at delivery"
// (read_sync); the backend decided which via wants_async().
template <reader_backend B>
class reader_engine {
 public:
  using window = file_reader::window;

  reader_engine(const std::filesystem::path& path,
                const file_reader_options& opts)
      // Window: power-of-2 multiple of the chunk size so every full window
      // is a subtree-aligned unit; >= 64 KiB keeps O_DIRECT alignment
      // trivial.
      : window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))),
        qd_(std::min(32u, std::max(2u, opts.queue_depth))),
        slots_(qd_),
        pool_(std::size_t{qd_} * window_),
        backend_(path, opts, qd_) {
    num_windows_ = (backend_.size() + window_ - 1) / window_;
    const std::uint64_t initial = std::min<std::uint64_t>(qd_, num_windows_);
    for (unsigned s = 0; s < initial; ++s) {
      assign(s);
    }
  }

  [[nodiscard]] std::uint64_t file_size() const noexcept {
    return backend_.size();
  }
  [[nodiscard]] const char* backend_name() const noexcept {
    return backend_.name();
  }

  std::optional<window> next() {
    if (next_deliver_ >= num_windows_) {
      return std::nullopt;
    }
    const std::uint64_t want = next_deliver_;
    unsigned s = 0;
    for (; s < qd_; ++s) {
      if (slots_[s].assigned && slots_[s].win == want) {
        break;
      }
    }
    assert(s < qd_);  // release() reassigns eagerly, so `want` has a slot
    slot_state& st = slots_[s];
    if (st.started) {
      backend_.wait(s);
    } else {
      backend_.read_sync(want * window_, {buf(s), st.target});
    }
    st.held = true;
    next_deliver_++;
    return window{buf(s), st.target, want * window_,
                  want + 1 == num_windows_, s};
  }

  void release(const window& w) noexcept {
    slot_state& st = slots_[w.slot];
    st.assigned = false;
    st.held = false;
    if (next_submit_ < num_windows_) {
      assign(w.slot);
    }
  }

 private:
  struct slot_state {
    std::uint64_t win = 0;   // window index assigned to this slot
    std::size_t target = 0;  // bytes this window must read
    bool assigned = false;
    bool started = false;  // async read in flight (wait) vs lazy (read_sync)
    bool held = false;     // delivered, not yet released
  };

  std::size_t window_len(std::uint64_t w) const noexcept {
    const std::uint64_t off = w * window_;
    const std::uint64_t rest = backend_.size() - off;
    return rest < window_ ? static_cast<std::size_t>(rest) : window_;
  }

  std::byte* buf(unsigned slot) const noexcept {
    return pool_.data + static_cast<std::size_t>(slot) * window_;
  }

  void assign(unsigned s) {
    slot_state& st = slots_[s];
    st.win = next_submit_++;
    st.target = window_len(st.win);
    st.assigned = true;
    st.held = false;
    st.started = backend_.wants_async(st.win * window_, st.target);
    if (st.started) {
      backend_.start(s, st.win * window_, {buf(s), st.target});
    }
    // Slots the backend declined are read synchronously at delivery time.
  }

  std::size_t window_;
  unsigned qd_;
  std::uint64_t num_windows_ = 0;
  std::uint64_t next_submit_ = 0;   // next window index to assign to a slot
  std::uint64_t next_deliver_ = 0;  // next window index to hand out
  std::vector<slot_state> slots_;

  // Declaration order is the teardown contract: the backend destructs
  // FIRST, draining any in-flight reads that target the pool, and the
  // pool is freed after. Do not reorder these two members.
  aligned_pool pool_;
  B backend_;
};

// The write-side slot engine, the reader's inverse: the producer fills
// buffers ahead of the device, and acquire() blocking on a slot whose
// write is still in flight is the entire backpressure story. The
// unaligned tail (only the final submit may be one) always goes through
// the backend's synchronous buffered path, because O_DIRECT and
// NO_BUFFERING both reject unaligned lengths; the backends that don't
// care route it the same way for uniformity.
template <writer_backend B>
class writer_engine {
 public:
  using buffer = file_writer::buffer;

  writer_engine(const std::filesystem::path& path,
                const file_writer_options& opts)
      : buffer_(rounded_buffer(opts.buffer_bytes)),
        qd_(std::min(32u, std::max(2u, opts.queue_depth))),
        pool_(std::size_t{qd_} * buffer_),
        backend_(path, opts, qd_) {}

  [[nodiscard]] std::uint64_t bytes_written() const noexcept {
    return written_;
  }
  [[nodiscard]] const char* backend_name() const noexcept {
    return backend_.name();
  }

  buffer acquire() {
    const unsigned s = next_slot_;
    backend_.wait_slot(s);
    return buffer{buf(s), buffer_, s};
  }

  void submit(const buffer& b, std::size_t bytes) {
    if (bytes == 0) {
      return;
    }
    if (tail_submitted_) {
      throw std::system_error(EINVAL, std::generic_category(),
                              "submit after partial write");
    }
    if (bytes % direct_align != 0) {
      tail_submitted_ = true;
    }
    const std::span<const std::byte> data{b.data, bytes};
    if (backend_.wants_async(bytes)) {
      backend_.start_write(b.slot, offset_, data);
    } else {
      backend_.write_sync(offset_, data);
    }
    offset_ += bytes;
    written_ += bytes;
    next_slot_ = (b.slot + 1) % qd_;
  }

  void finish() { backend_.finish(written_); }

 private:
  // Round up to the O_DIRECT length granule; >= 64 KiB so queued writes
  // are worth their submission cost.
  static std::size_t rounded_buffer(std::size_t bytes) noexcept {
    bytes = std::max<std::size_t>(bytes, 64 * 1024);
    return (bytes + direct_align - 1) / direct_align * direct_align;
  }

  std::byte* buf(unsigned slot) const noexcept {
    return pool_.data + static_cast<std::size_t>(slot) * buffer_;
  }

  std::size_t buffer_;
  unsigned qd_;
  unsigned next_slot_ = 0;       // round-robin acquire order
  std::uint64_t offset_ = 0;     // next sequential file offset
  std::uint64_t written_ = 0;    // total bytes accepted via submit()
  bool tail_submitted_ = false;  // a partial submit closes the stream

  // Declaration order is the teardown contract: the backend destructs
  // FIRST, draining any in-flight writes that read from the pool, and the
  // pool is freed after. Do not reorder these two members.
  aligned_pool pool_;
  B backend_;
};

}  // namespace blake3pp::detail::io_impl
