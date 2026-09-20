#pragma once

// The two portable I/O engines, written exactly once as class templates
// constrained by the concepts in io/backend.hpp: the reader over
// reader_context, the writer over writer_backend. The public
// file_reader/file_writer TUs instantiate them with what
// backend_select.hpp picks; the tests instantiate them again with the
// off-platform contexts and backends, so those stay compiled AND executed
// on every platform even though the selector never chooses them there.
// The constraint is the contract: an engine can only speak the concept's
// vocabulary, and a backend drifting from it fails at the instantiation
// with a diagnostic naming the missed requirement. Internal to src/io/,
// never installed.

#include <algorithm>
#include <concepts>
#include <string_view>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/file_writer.hpp>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

// The read-side window/slot engine on the completion-callback contract
// (reader_context in io/backend.hpp). Observably identical to
// reader_engine above: windows are delivered strictly in file order,
// release() recycles a slot and that recycling is the backpressure, and a
// submission that fails inside the noexcept release() is latched for the
// next next() to throw.
//
// What changed is where a read finishes. The context reports completion
// by calling back, only from inside poll(), so a slot is "ready" when its
// callback said so rather than when a blocking wait(slot) returned. next()
// therefore drives poll() until the window it wants is the one that
// arrived, absorbing the others on the way -- the same reordering the old
// wait() absorbed, moved to where a later phase can share one poll across
// several windows and, after that, several files.
template <reader_context C>
class polled_reader_engine {
 public:
  using window = file_reader::window;

  polled_reader_engine(const std::filesystem::path& path,
                       const file_reader_options& opts)
    requires std::constructible_from<typename C::file, C&,
                                     const std::filesystem::path&, bool>
      : window_(rounded_window_bytes(opts.window_bytes)),
        qd_(std::clamp(opts.queue_depth, 2u, max_queue_depth)),
        slots_(qd_),
        pool_(std::size_t{qd_} * window_),
        ctx_(reader_context_options{opts.async, opts.offload_submit}, qd_),
        file_(ctx_, path, opts.direct_io) {
    num_windows_ = (file_.size() + window_ - 1) / window_;
    const std::uint64_t initial = std::min<std::uint64_t>(qd_, num_windows_);
    for (unsigned s = 0; s < initial; ++s) {
      assign(s);
    }
    // One flush for the whole initial batch: the contract separates
    // queueing from submitting precisely so this is one io_uring_enter
    // rather than queue_depth of them.
    ctx_.flush();
  }

  // A source with no path: the null source is constructed from the size
  // it should pretend to have. Everything downstream -- window count,
  // slots, delivery order -- is the same machine.
  polled_reader_engine(std::uint64_t size, const file_reader_options& opts)
    requires std::constructible_from<typename C::file, C&, std::uint64_t>
      : window_(rounded_window_bytes(opts.window_bytes)),
        qd_(std::clamp(opts.queue_depth, 2u, max_queue_depth)),
        slots_(qd_),
        pool_(std::size_t{qd_} * window_),
        ctx_(reader_context_options{opts.async, opts.offload_submit}, qd_),
        file_(ctx_, size) {
    num_windows_ = (file_.size() + window_ - 1) / window_;
    const std::uint64_t initial = std::min<std::uint64_t>(qd_, num_windows_);
    for (unsigned s = 0; s < initial; ++s) {
      assign(s);
    }
    ctx_.flush();
  }

  [[nodiscard]] std::uint64_t file_size() const noexcept {
    return file_.size();
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
    return file_.name();
  }

  // The buffer arena, for callers that must fill it before the first read
  // (the null source hands back whatever is already there).
  [[nodiscard]] std::span<std::byte> pool() noexcept {
    return {pool_.data, std::size_t{qd_} * window_};
  }

  std::optional<window> next() {
    // See reader_engine::next(): release() is noexcept, so a failed
    // submission is latched there and thrown here.
    if (submit_failed_) {
      std::rethrow_exception(submit_failed_);
    }
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
    if (s == qd_) {
      throw std::system_error(EINVAL, std::generic_category(),
                              "next() with every window slot still held");
    }
    slot_state& st = slots_[s];
    while (!st.ready) {
      ctx_.poll(true);
    }
    if (st.ec) {
      throw std::system_error(st.ec, "read");
    }
    st.held = true;
    next_deliver_++;
    return window{buf(s), st.target, want * window_,
                  want + 1 == num_windows_, s};
  }

  void release(const window& w) noexcept {
    if (w.slot >= qd_ || !slots_[w.slot].held) {
      if (!submit_failed_) {
        submit_failed_ = std::make_exception_ptr(std::system_error(
            EINVAL, std::generic_category(),
            "release() of a window this reader did not hand out"));
      }
      return;
    }
    slot_state& st = slots_[w.slot];
    st.assigned = false;
    st.held = false;
    if (next_submit_ < num_windows_ && !submit_failed_) {
      try {
        assign(w.slot);
        ctx_.flush();
      } catch (...) {
        submit_failed_ = std::current_exception();
      }
    }
  }

 private:
  struct slot_state {
    typename C::read_op op{};
    std::uint64_t win = 0;
    std::size_t target = 0;
    std::error_code ec{};
    bool assigned = false;
    bool ready = false;  // the callback has run for this window
    bool held = false;   // delivered, not yet released
  };

  // The one place a context hands control back. Marking the slot is all
  // it may do: the engine is not reentrant, and the contract's promise is
  // that this runs on the thread inside poll().
  static void on_done(read_op_base* base, std::error_code ec) noexcept {
    auto* const st = static_cast<slot_state*>(base->owner);
    st->ec = ec;
    st->ready = true;
  }

  std::size_t window_len(std::uint64_t w) const noexcept {
    const std::uint64_t off = w * window_;
    const std::uint64_t rest = file_.size() - off;
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
    st.ready = false;
    st.ec = {};
    st.op.done = &on_done;
    st.op.owner = &st;
    ctx_.submit_read(file_, st.win * window_, {buf(s), st.target}, st.op);
  }

  std::size_t window_;
  unsigned qd_;
  std::uint64_t num_windows_ = 0;
  std::uint64_t next_submit_ = 0;
  std::uint64_t next_deliver_ = 0;
  std::exception_ptr submit_failed_;
  std::vector<slot_state> slots_;

  // Declaration order is the teardown contract, one link longer than the
  // old engine's: the file closes first, then the context drains every
  // read still owed (running no callback), and only then is the pool it
  // was reading into freed. Do not reorder these three members.
  aligned_pool pool_;
  C ctx_;
  typename C::file file_;
};

// The write-side slot engine, the reader's inverse. The producer fills
// buffers ahead of the device, and acquire() blocking on a slot whose write
// is still in flight is the entire backpressure story.
//
// The unaligned tail, and only the final submit may be one, always goes
// through the backend's synchronous buffered path, because O_DIRECT and
// NO_BUFFERING both reject unaligned lengths. The backends that do not care
// route it the same way, for uniformity.
template <writer_backend B>
class writer_engine {
 public:
  using buffer = file_writer::buffer;

  writer_engine(const std::filesystem::path& path,
                const file_writer_options& opts)
      : buffer_(rounded_buffer(opts.buffer_bytes)),
        qd_(std::clamp(opts.queue_depth, 2u, max_queue_depth)),
        pool_(std::size_t{qd_} * buffer_),
        backend_(path, opts, qd_) {}

  [[nodiscard]] std::uint64_t bytes_written() const noexcept {
    return written_;
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
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
    if (b.slot >= qd_ || bytes > buffer_) {
      throw std::system_error(EINVAL, std::generic_category(),
                              "submit() of a buffer this writer did not "
                              "hand out, or of more bytes than it holds");
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
    bytes = std::clamp<std::size_t>(bytes, 64 * 1024,
                                    max_window_bytes / direct_align *
                                        direct_align);
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
