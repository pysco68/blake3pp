#pragma once

// The io_context of the synchronous sources, pread and stdio: every
// transfer goes to the lanes.
// Internal to src/io/, never installed.

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>

#include <blake3pp/detail/driver.hpp>

#include "io/engine_rung.hpp"
#include "io/lanes.hpp"
#include "io/options.hpp"
#include "io/transfer.hpp"
#include "io/waiter.hpp"

namespace blake3pp::detail::io_impl {

// The context of every synchronous source. submit_read() and
// submit_write() only queue. A lane of the pump performs the transfer,
// and poll() runs the callbacks of what the lanes finished. The source
// opens a path, knows its size and name, reads and writes a span at an
// offset, and sets the size. pread and stdio differ only there.
//
// A source says whether it can serve two transfers at once through
// Source::positional. pread and pwrite can, because they carry their own
// offset. stdio cannot, because seek-then-read is two calls over one
// cursor. That source gets a single lane, and its transfers run one at a
// time.
template <class Source>
class sync_context {
  static_assert(std::same_as<decltype(Source::positional), const bool>,
                "a synchronous source must declare whether its transfers "
                "may run concurrently: static constexpr bool positional");

 public:
  class file;

  struct transfer : performed_transfer {
    std::span<std::byte> buf{};        // reads fill this
    std::span<const std::byte> wbuf{};  // writes read from this
    std::uint64_t off = 0;
    Source* src = nullptr;
    file* owner_file = nullptr;  // whose transfer this is, for forget()
    transfer* next_deferred = nullptr;
    bool cancelled = false;
    bool is_write = false;
  };

  class file {
   public:
    file(sync_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : ctx_(&ctx), src_(path, how) {}

    // Queued transfers name this file, so they go with it, and only
    // they: forget() drops this file's queued and finished transfers and
    // waits out the lanes, and a sibling's stay to be reported. The
    // operations must still be alive here (see the contract's operation
    // lifetime rule): a lane may be writing into one until forget()
    // returns. A count a context-wide drain left stale costs one such
    // wait and nothing else.
    ~file() {
      if (inflight_ != 0) {
        ctx_->forget(*this);
      }
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return src_.size(); }
    [[nodiscard]] std::string_view name() const noexcept {
      return src_.name();
    }
    void set_size(std::uint64_t bytes) { src_.set_size(bytes); }

   private:
    friend class sync_context;
    sync_context* ctx_;
    Source src_;
    // This file's share of the context's count, so the destructor can tell
    // whether anything of its own is queued without walking the queue.
    std::size_t inflight_ = 0;
  };

  // The rung this is, declared for the ladder walk in
  // src/io/io_engines.cpp. A synchronous context has nothing under it and
  // nothing can refuse it, so its verdict is always settled. The source
  // names the engine, as its name() spells the file's backend string.
  static constexpr io_engine rung_engine = Source::rung_engine;
  using rung_below = void;
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return {rung_engine, true};
  }

  // The depth the caller asked for decides how many threads serve the
  // queue, since more threads than transfers in flight can never help.
  // A source that cannot take two at once gets one whatever the depth.
  sync_context(const reader_context_options&, unsigned max_inflight) noexcept
      : pump_(io_lanes::waker<sync_context>(), this, &perform,
              Source::positional ? io_lanes_for(max_inflight) : 1u) {}
  sync_context(const sync_context&) = delete;
  sync_context& operator=(const sync_context&) = delete;
  // Drains before any member goes, so no lane is inside a transfer once
  // this returns and none takes another: the operations, which the
  // contract keeps alive until here, are not touched after it. The
  // pump's own destructor then joins its idle lanes; it is declared last,
  // so the waiter its lanes signal outlives the join. Every file must
  // already be gone, and each one's destructor drained what it owed.
  ~sync_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    op.buf = buf;
    op.wbuf = {};
    op.off = off;
    op.src = &f.src_;
    op.owner_file = &f;
    op.cancelled = false;
    op.is_write = false;
    op.reset_performance();
    in_flight_++;
    f.inflight_++;
    pump_.push(op);
  }

  // Handed to the pump like a read; a lane performs it and the callback
  // runs inside poll().
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, transfer& op) noexcept {
    op.buf = {};
    op.wbuf = buf;
    op.off = off;
    op.src = &f.src_;
    op.owner_file = &f;
    op.cancelled = false;
    op.is_write = true;
    op.reset_performance();
    in_flight_++;
    f.inflight_++;
    pump_.push(op);
  }

  // The pump was given the transfer when it was submitted, so there is
  // no batch to push. The contract keeps the call because io_uring has
  // one.
  void flush() noexcept {}

  // Gives up this file's transfers without reporting them. What is still
  // queued was never performed and is dropped; what a lane holds is
  // waited out, because it is reading or writing through this file's
  // descriptor and into the caller's buffer. Legal only while the
  // operations are alive; the file's destructor calls it.
  //
  // The lanes are the context's, so the wait covers sibling files too,
  // and a sibling's finished transfer stays on the done list for a later
  // poll() to report.
  void forget(file& f) noexcept {
    const auto mine = [&f](const transfer& op) noexcept {
      return op.owner_file == &f;
    };
    std::size_t dropped = pump_.remove_queued_if(mine);
    pump_.quiesce();
    dropped += pump_.remove_done_if(mine);
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
  }

  std::size_t poll(bool block) {
    std::size_t ran = serve_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (waiter_.take()) {
        // A wake from another thread or from a lane; the done list says
        // which.
        return ran + serve_deferred();
      }
      waiter_.sleep();
      ran += serve_deferred();
      if (ran > 0) {
        return ran;
      }
    }
  }

  void wake() noexcept { waiter_.wake(); }

  // A transfer no lane has taken yet is flagged and reports
  // operation_canceled; one a lane already has runs to completion.
  void cancel(transfer& op) noexcept { pump_.cancel(op); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Waits the lanes out, then forgets everything queued and everything
  // finished: none of it is reported. The files' own counts are left as
  // they were; see ~file().
  void drain() noexcept {
    pump_.clear();
    in_flight_ = 0;
  }

  // Every transfer rides the pump here, so its lanes ARE the device's
  // queue and its stamps are the whole of the device's time.
  [[nodiscard]] unsigned device_depth() const noexcept {
    return pump_.depth();
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }

 private:
  // One transfer, on a lane. Touches the operation and the source it
  // names, and nothing else of this context.
  static std::error_code perform(transfer& op) noexcept {
    try {
      if (op.is_write) {
        op.src->write_at(op.off, op.wbuf);
      } else {
        op.src->read_at(op.off, op.buf);
      }
    } catch (const std::system_error& e) {
      return e.code();
    } catch (...) {
      // A source throws only system_error; anything else is reported as
      // EIO so it cannot terminate a noexcept lane.
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  // Runs the callbacks of every transfer the lanes finished, on this
  // thread. The inline step serves a context whose lanes could not start.
  std::size_t serve_deferred() noexcept {
    return pump_.report_finished(
        [this](transfer& op) noexcept { report(op); });
  }

  void report(transfer& op) noexcept {
    in_flight_--;
    assert(op.owner_file != nullptr &&
           "every transfer is queued with its file");
    op.owner_file->inflight_--;
    device_.add_if_served(op.device_total_ns());
    op.done(&op, op.result());
  }

  std::size_t in_flight_ = 0;
  // What the lanes measured around their own transfers.
  device_time device_{};
  poll_waiter waiter_;
  // Last, so its destructor joins the lanes while the waiter they signal
  // is still alive. Do not move it above waiter_.
  deferred_pump<transfer> pump_;
};

}  // namespace blake3pp::detail::io_impl
