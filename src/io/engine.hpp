#pragma once

// The two portable I/O engines, written exactly once as class templates
// constrained by io_context, the concept in io/io_context.hpp. The public
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

#include "io/io_context.hpp"
#include "io/options.hpp"

namespace blake3pp::detail::io_impl {

// The read-side window/slot engine on the completion-callback contract
// (io_context in io/io_context.hpp): windows are delivered strictly in
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
    return window{{buf(s).data(), st.target}, want * window_,
                  want + 1 == num_windows_, s};
  }

  // The slot AND the window in it: a window object names a slot, and a
  // slot is reassigned the moment it is released, so the index alone
  // cannot tell the window being handed back from one this engine already
  // recycled. Releasing a stale window twice would hand its buffer to the
  // next read while the caller is still hashing the window that came
  // after it -- a wrong digest with nothing to see. The offset is the
  // window's identity, and the slot remembers which window it holds.
  void release(const window& w) noexcept {
    if (w.slot >= qd_ || !slots_[w.slot].held ||
        slots_[w.slot].win * window_ != w.offset) {
      if (!submit_failed_) {
        submit_failed_ = std::make_exception_ptr(std::system_error(
            EINVAL, std::generic_category(),
            "release() of a window this reader did not hand out, or of one "
            "it has already taken back"));
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
    typename C::transfer op{};
    std::uint64_t win = 0;
    std::size_t target = 0;
    std::error_code ec{};
    bool assigned = false;
    bool ready = false;  // the callback has run for this window
    bool held = false;   // delivered, not yet released

    // The one place a context hands control back. Marking the slot is all
    // it may do: the engine is not reentrant, and the contract's promise
    // is that this runs on the thread inside poll().
    void complete(std::error_code e) noexcept {
      ec = e;
      ready = true;
    }
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
        ctx_(reader_context_options{.async = opts.async,
                                    .offload_submit = opts.offload_submit,
                                    .ioring = opts.ioring},
             qd_),
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



  std::size_t window_len(std::uint64_t w) const noexcept {
    const std::uint64_t off = w * window_;
    const std::uint64_t rest = file_.size() - off;
    return rest < window_ ? static_cast<std::size_t>(rest) : window_;
  }

  [[nodiscard]] std::span<std::byte> buf(unsigned slot) const noexcept {
    return {pool_.get() + static_cast<std::size_t>(slot) * window_, window_};
  }

  void assign(unsigned s) {
    slot_state& st = slots_[s];
    st.win = next_submit_++;
    st.target = window_len(st.win);
    st.assigned = true;
    st.held = false;
    st.ready = false;
    st.ec = {};
    bind_completion<&slot_state::complete>(st.op, st);
    ctx_.submit_read(file_, st.win * window_, buf(s).first(st.target), st.op);
  }

  std::size_t window_;
  unsigned qd_;
  std::uint64_t num_windows_ = 0;
  std::uint64_t next_submit_ = 0;
  std::uint64_t next_deliver_ = 0;
  std::exception_ptr submit_failed_;
  std::vector<slot_state> slots_;

  // Declaration order is the teardown contract: the file closes first,
  // then the context drains every read still owed (running no callback),
  // and only then is the pool it was reading into freed. slots_, declared
  // above them, goes last of all: it holds the operations the context may
  // still own until its drain returns. Do not reorder these four members.
  aligned_buffer pool_;
  C ctx_;
  typename C::file file_;
};

// The write-side slot engine, the reader's inverse, over the same
// io_context: the producer fills buffers ahead of the device, and
// acquire() blocking on a slot whose write is still in flight is the
// backpressure.
//
// Every submit queues; the context decides how the bytes travel. A
// length the platform cannot take asynchronously is performed on one of
// the backend's I/O lanes, and its completion is reported from poll(),
// exactly as such a read's is.
// Hence the timing both paths share:
//
//   submit() ACCEPTS bytes. finish() LANDS them.
//   A WRITE error surfaces from acquire() or finish(), never from submit();
//   a broken contract throws where it is detected.
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
        ctx_(reader_context_options{.async = opts.async,
                                    .offload_submit = opts.offload_submit,
                                    .ioring = opts.ioring},
             qd_),
        file_(ctx_, path,
              file_open_options{.write = true,
                                .direct_io = opts.direct_io,
                                .preallocate_bytes = opts.preallocate_bytes,
                                .waive_zero_fill = opts.waive_zero_fill}) {}

  [[nodiscard]] std::uint64_t bytes_written() const noexcept {
    return written_;
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
    return file_.name();
  }

  // Round-robin: the next slot in order, waited out. The slot is claimed
  // here rather than at submit, so two buffers in hand are two slots: with
  // the index alone deciding, a second acquire() before the first submit
  // handed out the same buffer twice, both writes rode one busy flag, and
  // the producer refilled bytes the device was still reading.
  buffer acquire() {
    // Round-robin from the last hand-out, but past any slot the producer
    // still holds: submits may come back out of order, and a free slot is a
    // buffer this call can hand out. Only every slot held is a refusal.
    unsigned s = next_slot_;
    for (unsigned tried = 0; slots_[s].acquired; ++tried) {
      if (tried + 1 == qd_) {
        throw std::system_error(
            EINVAL, std::generic_category(),
            "acquire() with every buffer slot already held");
      }
      s = (s + 1) % qd_;
    }
    wait_slot(s);
    throw_if_failed();
    slots_[s].acquired = true;
    next_slot_ = (s + 1) % qd_;
    return buffer{{buf(s).data(), buffer_}, s};
  }

  void submit(const buffer& b, std::size_t bytes) {
    if (b.slot >= qd_ || bytes > buffer_ || !slots_[b.slot].acquired) {
      throw std::system_error(EINVAL, std::generic_category(),
                              "submit() of a buffer this writer did not "
                              "hand out, or has already taken back, or of "
                              "more bytes than it holds");
    }
    if (bytes == 0) {
      // Nothing to write, so the slot goes back to the front of the ring
      // rather than being skipped: a generator that produces nothing on its
      // last call must not cost a buffer.
      slots_[b.slot].acquired = false;
      next_slot_ = b.slot;
      return;
    }
    if (tail_submitted_) {
      throw std::system_error(EINVAL, std::generic_category(),
                              "submit after partial write");
    }
    if (bytes % direct_align != 0) {
      tail_submitted_ = true;
    }
    slot_state& st = slots_[b.slot];
    st.acquired = false;
    st.busy = true;
    st.ec = {};
    bind_completion<&slot_state::complete>(st.op, st);
    ctx_.submit_write(file_, offset_, b.bytes.first(bytes), st.op);
    ctx_.flush();
    offset_ += bytes;
    written_ += bytes;
  }

  // Lands everything accepted, then reports the first error any of it
  // produced. No byte a submit() accepted is dropped on the way.
  void finish() {
    for (unsigned s = 0; s < qd_; ++s) {
      wait_slot(s);
    }
    throw_if_failed();
    // Always, not only to cut a preallocation back: set_size() is also
    // where a buffered backend lands what it still holds (stdio's is a
    // flush), and "finish() lands" has to hold for every backend. On
    // the others it is one truncate to the size the file already has.
    file_.set_size(written_);
  }

 private:
  struct slot_state {
    typename C::transfer op{};
    std::error_code ec{};
    bool busy = false;      // a write is in flight in this slot
    bool acquired = false;  // handed out, not yet submitted

    // The one place the context hands control back. Marking the slot is
    // all it may do: the engine is not reentrant.
    void complete(std::error_code e) noexcept {
      ec = e;
      busy = false;
    }
  };

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

  // Round up to the O_DIRECT length granule; >= min_window_bytes so queued
  // writes are worth their submission cost.
  static std::size_t rounded_buffer(std::size_t bytes) noexcept {
    bytes = std::clamp<std::size_t>(bytes, min_window_bytes,
                                    max_window_bytes / direct_align *
                                        direct_align);
    return (bytes + direct_align - 1) / direct_align * direct_align;
  }

  [[nodiscard]] std::span<std::byte> buf(unsigned slot) const noexcept {
    return {pool_.get() + static_cast<std::size_t>(slot) * buffer_, buffer_};
  }

  std::size_t buffer_;
  unsigned qd_;
  unsigned next_slot_ = 0;       // round-robin acquire order
  std::uint64_t offset_ = 0;     // next sequential file offset
  std::uint64_t written_ = 0;    // total bytes accepted via submit()
  std::error_code failed_{};     // the first error, until it is reported
  bool tail_submitted_ = false;  // a partial submit closes the stream
  std::vector<slot_state> slots_;

  // Declaration order is the teardown contract: the file closes first,
  // then the context drains every write still owed (running no callback),
  // and only then is the pool it was writing from freed. slots_, declared
  // above them, goes last of all: it holds the operations the context may
  // still own until its drain returns. Do not reorder these four members.
  aligned_buffer pool_;
  C ctx_;
  typename C::file file_;
};

}  // namespace blake3pp::detail::io_impl
