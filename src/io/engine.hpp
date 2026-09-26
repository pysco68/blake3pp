#pragma once

// The two portable I/O engines, written exactly once as class templates
// constrained by the concepts in io/backend.hpp: the reader over
// io_context, the writer over writer_backend. The public
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
#include <utility>
#include <vector>

#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/file_writer.hpp>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

// The read-side window/slot engine on the completion-callback contract
// (io_context in io/backend.hpp): windows are delivered strictly in
// file order, release() recycles a slot and that recycling is the
// backpressure, and a submission that fails inside the noexcept
// release() is latched for the next next() to throw.
//
// A read finishes when the context calls back, only from inside poll(),
// so a slot is "ready" when its callback said so. next() therefore
// drives poll() until the window it wants is the one that arrived,
// absorbing the others on the way; one poll serves every window in
// flight, which is what lets the file pipeline share it across several.
template <io_context C>
class polled_reader_engine {
 public:
  using window = file_reader::window;

  polled_reader_engine(const std::filesystem::path& path,
                       const file_reader_options& opts)
    requires std::constructible_from<typename C::file, C&,
                                     const std::filesystem::path&,
                                     const file_open_options&>
      : polled_reader_engine(over_file{}, opts, path,
                             file_open_options{.direct_io = opts.direct_io}) {}

  // A source with no path: the null source is constructed from the size
  // it should pretend to have. Everything downstream -- window count,
  // slots, delivery order -- is the same machine.
  polled_reader_engine(std::uint64_t size, const file_reader_options& opts)
    requires std::constructible_from<typename C::file, C&, std::uint64_t>
      : polled_reader_engine(over_file{}, opts, size) {}

  [[nodiscard]] std::uint64_t file_size() const noexcept {
    return file_.size();
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
    return file_.name();
  }

  // The buffer arena, for callers that must fill it before the first read
  // (the null source hands back whatever is already there).
  [[nodiscard]] std::span<std::byte> pool() noexcept {
    return {pool_.get(), std::size_t{qd_} * window_};
  }

  std::optional<window> next() {
    // release() is noexcept, so a failed submission is latched there and
    // thrown here.
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

  // The engine behind both public constructors: the file is whatever the
  // context makes of the arguments after opts, and the first batch of
  // reads goes out before the constructor returns.
  struct over_file {};
  template <class... FileArgs>
  polled_reader_engine(over_file, const file_reader_options& opts,
                       FileArgs&&... file_args)
      : window_(rounded_window_bytes(opts.window_bytes)),
        qd_(clamp_queue_depth(opts.queue_depth)),
        slots_(qd_),
        pool_(make_aligned_buffer(std::size_t{qd_} * window_)),
        ctx_(reader_context_options{opts.async, opts.offload_submit}, qd_),
        file_(ctx_, std::forward<FileArgs>(file_args)...) {
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
    return pool_.get() + static_cast<std::size_t>(slot) * window_;
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
  aligned_buffer pool_;
  C ctx_;
  typename C::file file_;
};

// The write-side slot engine, the reader's inverse, over the same
// io_context: the producer fills buffers ahead of the device, and
// acquire() blocking on a slot whose write is still in flight is the
// entire backpressure story.
//
// What changed when this moved onto the contract. The old engine asked
// the backend wants_async() and, when the answer was no, wrote
// synchronously inside submit(). Now every submit queues and the context
// decides; a length the platform cannot take asynchronously rides the
// backend's deferred list and lands inside poll(), where a read that
// cannot be issued asynchronously already landed. So:
//
//   submit() ACCEPTS bytes. finish() LANDS them.
//   Errors surface from acquire() or finish(), never from submit().
//
// That is the same contract the async path always had -- its errors
// surfaced from acquire() too -- so the two paths now report alike
// rather than one throwing early and the other late.
template <io_context C>
class polled_writer_engine {
 public:
  using buffer = file_writer::buffer;

  polled_writer_engine(const std::filesystem::path& path,
                       const file_writer_options& opts)
      : buffer_(rounded_buffer(opts.buffer_bytes)),
        qd_(clamp_queue_depth(opts.queue_depth)),
        slots_(qd_),
        pool_(make_aligned_buffer(std::size_t{qd_} * buffer_)),
        ctx_(reader_context_options{opts.async, opts.offload_submit}, qd_),
        file_(ctx_, path,
              file_open_options{.write = true,
                                .direct_io = opts.direct_io,
                                .preallocate_bytes = opts.preallocate_bytes}),
        preallocated_(opts.preallocate_bytes) {}

  [[nodiscard]] std::uint64_t bytes_written() const noexcept {
    return written_;
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
    return file_.name();
  }

  // Round-robin, as before: the next slot in order, waited out.
  buffer acquire() {
    const unsigned s = next_slot_;
    wait_slot(s);
    throw_if_failed();
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
    slot_state& st = slots_[b.slot];
    st.busy = true;
    st.ec = {};
    st.op.done = &on_done;
    st.op.owner = &st;
    ctx_.submit_write(file_, offset_, {b.data, bytes}, st.op);
    ctx_.flush();
    offset_ += bytes;
    written_ += bytes;
    next_slot_ = (b.slot + 1) % qd_;
  }

  // Lands everything accepted, then reports the first error any of it
  // produced. No byte a submit() accepted is dropped on the way.
  void finish() {
    for (unsigned s = 0; s < qd_; ++s) {
      wait_slot(s);
    }
    throw_if_failed();
    if (preallocated_ > written_) {
      file_.set_size(written_);
      preallocated_ = 0;
    }
  }

 private:
  struct slot_state {
    typename C::read_op op{};
    std::error_code ec{};
    bool busy = false;
  };

  // The one place the context hands control back. Marking the slot is all
  // it may do: the engine is not reentrant.
  static void on_done(read_op_base* base, std::error_code ec) noexcept {
    auto* const st = static_cast<slot_state*>(base->owner);
    st->ec = ec;
    st->busy = false;
  }

  void wait_slot(unsigned s) {
    while (slots_[s].busy) {
      ctx_.poll(true);
    }
    // The first error wins and is remembered until someone asks, so a
    // failure can never be lost between a submit that accepted it and
    // the acquire or finish that reports it.
    if (slots_[s].ec && !failed_) {
      failed_ = slots_[s].ec;
    }
  }

  void throw_if_failed() {
    if (failed_) {
      const std::error_code ec = failed_;
      failed_ = {};
      throw std::system_error(ec, "blake3pp: writing a file");
    }
  }

  // Round up to the O_DIRECT length granule; >= 64 KiB so queued writes
  // are worth their submission cost.
  static std::size_t rounded_buffer(std::size_t bytes) noexcept {
    bytes = std::clamp<std::size_t>(bytes, 64 * 1024,
                                    max_window_bytes / direct_align *
                                        direct_align);
    return (bytes + direct_align - 1) / direct_align * direct_align;
  }

  std::byte* buf(unsigned slot) const noexcept {
    return pool_.get() + static_cast<std::size_t>(slot) * buffer_;
  }

  std::size_t buffer_;
  unsigned qd_;
  unsigned next_slot_ = 0;       // round-robin acquire order
  std::uint64_t offset_ = 0;     // next sequential file offset
  std::uint64_t written_ = 0;    // total bytes accepted via submit()
  std::uint64_t preallocated_ = 0;
  std::error_code failed_{};     // the first error, until it is reported
  bool tail_submitted_ = false;  // a partial submit closes the stream
  std::vector<slot_state> slots_;

  // Declaration order is the teardown contract: the file closes first,
  // then the context drains every write still owed (running no callback),
  // and only then is the pool it was writing from freed. Do not reorder
  // these three members.
  aligned_buffer pool_;
  C ctx_;
  typename C::file file_;
};

}  // namespace blake3pp::detail::io_impl
