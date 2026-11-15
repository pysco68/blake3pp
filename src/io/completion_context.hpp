#pragma once

// One io_context for every backend whose kernel completes transfers on a
// queue of its own: io_uring, POSIX AIO over kqueue, the completion port
// and the I/O ring -- and libdispatch, whose worker pool completes them
// the same way. The context is written once here -- the files and
// their list, the counts, the deferred pump, what a completion means,
// how poll() waits, how a closing file and a closing context wait out
// what the kernel still holds. A backend supplies only its engine: how a
// piece goes to the kernel, how a completion comes back, how a sleeping
// poll is woken, and what it does when the kernel refuses a submission.
// Internal to src/io/, never installed.
//
// The engine a context is instantiated with provides:
//
//   struct transfer_part;   // the transfer's base: performed_transfer,
//                           // the buffers, off, filled, length(),
//                           // cancelled, is_write, and
//                           //   arm_read(file_part&, off, span<byte>)
//                           //   arm_write(file_part&, off,
//                           //             span<const byte>)
//   struct file_part;       // the file's OS layer, built as
//                           //   file_part(Engine&, path, how)
//                           // with size(), name(), set_size()
//   static constexpr io_engine rung_engine; using rung_below;
//   std::string_view mechanism() const;  // named in failures
//   Engine(const reader_context_options&, unsigned max_inflight);
//   bool engaged() const;             // the kernel queue was had
//   rung_verdict engaged_rung() const;
//   unsigned device_depth(bool used, unsigned lanes) const;
//   static std::error_code perform(Op&) noexcept;   // a lane's transfer
//   bool place(Op&) noexcept;         // the rest of op, from op.filled,
//                                     // onto the kernel queue; false
//                                     // leaves it holding nothing
//   void cancel(Op&) noexcept;        // op is on the kernel queue
//   void flush<Op>(Refused) noexcept; // submit; what the kernel will not
//                                     // take goes to refused(op). After
//                                     // it, whatever is still unsubmitted
//                                     // is backed by work the kernel or the
//                                     // pump will complete, or a failure is
//                                     // latched: a wait can always end.
//   void take_back<Op>(Refused) noexcept;  // everything not submitted
//   std::error_code take_failure() noexcept;  // a latched submit failure
//   engine_event<Op> next<Op>(bool block) noexcept;
//   void wake() noexcept;             // any thread; next() reports it,
//                                     // next(false) before it says empty
//   void cancel_file(file_part&, std::size_t owed) noexcept;
//   std::size_t drop_unsubmitted<Op>(Pred mine) noexcept;
//   std::size_t kernel_owed() const noexcept;  // requests the kernel holds,
//                                      // its own cancel and wake included
//   void begin_drain() noexcept;  void end_drain() noexcept;
//                                     // around the drain's wait
//   void release(Op&) noexcept;       // what the engine lent the op
//
// where Op is the context's transfer, so the engine's members are
// templates over it.

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <blake3pp/detail/driver.hpp>

#include "io/ladder.hpp"
#include "io/lanes.hpp"
#include "io/options.hpp"

namespace blake3pp::detail::io_impl {

// What an engine hands back from one look at its completion queue. Its
// own sentinels (a cancel's completion, a wake poll, a request a drain
// forgot) are consumed inside it and come back as `progress`: nothing for
// the caller, but what the kernel owes may have dropped, which is what a
// drain waits on. A blocking look returns once something happened.
template <class Op>
struct engine_event {
  enum class kind { completion, progress, wake, empty, failed };
  kind what = kind::empty;
  Op* op = nullptr;      // completion: the transfer one piece of was
  std::error_code ec{};  // completion: its error; failed: the queue's own
  std::size_t moved = 0;  // completion: the bytes the piece moved
};

template <class Engine>
class completion_context {
 public:
  class file;

  struct transfer : Engine::transfer_part {
    file* owner_file = nullptr;          // whose transfer this is
    transfer* next_deferred = nullptr;   // the pump's one link
    // The engine holds it: queued for the kernel or in the kernel's hands,
    // so a cancel goes to the engine. False while the pump has it.
    bool engine_held = false;
  };
  using event = engine_event<transfer>;

  class file {
   public:
    file(completion_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : part_(ctx.engine_, path, how), ctx_(&ctx) {
      ctx.adopt(*this);
    }
    // Reclaimed, then off the context's list: forget() leaves a file on
    // it, because a file that is merely idled is still open and may be
    // read again. This one is not -- and a destroyed file still on the
    // list is what a later drain() would walk.
    ~file() {
      ctx_->forget(*this);
      ctx_->unlink(*this);
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return part_.size(); }
    [[nodiscard]] std::string_view name() const noexcept {
      return part_.name();
    }
    void set_size(std::uint64_t bytes) { part_.set_size(bytes); }

    // The engine's half, which its members reach through the transfer.
    [[nodiscard]] typename Engine::file_part& part() noexcept { return part_; }
    [[nodiscard]] const typename Engine::file_part& part() const noexcept {
      return part_;
    }

   private:
    friend class completion_context;
    typename Engine::file_part part_;
    completion_context* ctx_;
    file* next_ = nullptr;      // the context's list of open files
    std::size_t inflight_ = 0;  // this file's share of the context's count
  };

  static constexpr io_engine rung_engine = Engine::rung_engine;
  using rung_below = typename Engine::rung_below;

  completion_context(const reader_context_options& opts,
                     unsigned max_inflight)
      : engine_(opts, max_inflight),
        // Declared last, and initialized last: see the member list.
        pump_(io_lanes::waker<completion_context>(), this, &perform_deferred,
              io_lanes_for(max_inflight)) {}
  completion_context(const completion_context&) = delete;
  completion_context& operator=(const completion_context&) = delete;

  // Every file is destroyed first, and each one's destructor forgot its
  // own transfers, so the drain finds nothing owed.
  ~completion_context() { drain(); }

  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return engine_.engaged_rung();
  }
  [[nodiscard]] bool engaged() const noexcept { return engine_.engaged(); }
  [[nodiscard]] Engine& engine() noexcept { return engine_; }
  [[nodiscard]] const Engine& engine() const noexcept { return engine_; }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    arm(op, f);
    op.arm_read(f.part_, off, buf);
    place(op);
  }
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, transfer& op) noexcept {
    arm(op, f);
    op.arm_write(f.part_, off, buf);
    place(op);
  }

  void flush() noexcept { engine_.template flush<transfer>(to_pump()); }

  // The kernel is asked once; a second ask would only repeat the
  // question. A transfer the pump has is cancelled through the pump,
  // under its lock, because the flag is what a lane reads when it takes
  // one; one a lane already has runs to completion.
  void cancel(transfer& op) noexcept {
    if (!op.engine_held) {
      pump_.cancel(op);
      return;
    }
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    engine_.cancel(op);
  }

  // The kernel queue first, then the pump's finished list: a completion
  // can move its remainder to the pump, and the list is what serves it.
  std::size_t poll(bool block) {
    submit_or_throw();
    const std::size_t before = reported_;
    reap(false, before);
    serve_deferred();
    if (reported_ != before || !block) {
      return reported_ - before;
    }
    for (;;) {
      if (take_wake()) {
        serve_deferred();
        return reported_ - before;
      }
      reap(true, before);
      serve_deferred();
      if (reported_ != before || take_wake()) {
        return reported_ - before;
      }
    }
  }

  // The one member another thread may call, and how a lane reports a
  // finished transfer.
  void wake() noexcept { engine_.wake(); }

  // Transfers the caller is still owed a callback for, wherever they are:
  // queued for the kernel, in its hands, on the pump. A drain or a file's
  // forget() takes some off without a callback.
  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }
  // Transfers, or remainders of transfers, performed by the lanes (or by
  // poll() where no lane runs) and reported since construction.
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return served_synchronously_;
  }
  [[nodiscard]] unsigned device_depth() const noexcept {
    return engine_.device_depth(used_engine_, pump_.depth());
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }

  // Everything the kernel has not been given goes to the pump: what a
  // submission the kernel refused outright leaves behind. Called by the
  // tests that cover that path, since a working kernel cannot be asked to
  // refuse.
  void defer_unsubmitted() noexcept {
    engine_.template take_back<transfer>(to_pump());
  }

  // Waits out every request the kernel holds and reports none of it,
  // then clears the pump, waiting its lanes out. A failure to wait means
  // the kernel can never give the requests back, and the caller frees
  // the buffers they name as soon as this returns, so the process ends
  // with the reason instead.
  void drain() noexcept {
    engine_.begin_drain();
    for (file* f = files_; f != nullptr; f = f->next_) {
      engine_.cancel_file(f->part_, f->inflight_);
    }
    if (const std::error_code ec = engine_.take_failure()) {
      lost("", engine_.kernel_owed(), ec);
    }
    while (engine_.kernel_owed() > 0) {
      const event e = engine_.template next<transfer>(true);
      switch (e.what) {
        case event::kind::completion:  // reaped, not reported
        case event::kind::progress:
          break;
        case event::kind::wake:
          woken_ = true;
          break;
        case event::kind::empty:
        case event::kind::failed:
          lost("", engine_.kernel_owed(), e.ec);
      }
    }
    engine_.end_drain();
    pump_.clear();
    in_flight_ = 0;
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->inflight_ = 0;
    }
  }

  // Gives up every transfer this file owns, running no callback, so the
  // file and the buffers its transfers name may be destroyed while the
  // context stays usable. The file's destructor calls this. Legal only
  // while the operations are alive.
  //
  // What never reached the kernel is dropped. What the kernel or a lane
  // holds is waited out, because it may be writing into those buffers.
  // The queue is shared, so a sibling's completion can surface meanwhile;
  // it is reported, since dropping it would strand an operation that can
  // never complete. The file stays on the list: it is still open and may
  // be read again.
  void forget(file& f) noexcept {
    // A file that owes nothing has nothing queued, on a lane or in the
    // kernel, so it closes without waiting for its siblings' lanes.
    if (f.inflight_ == 0) {
      return;
    }
    sweep(f);
    if (!engine_.engaged() || f.inflight_ == 0) {
      return;
    }
    // Everything built goes to the kernel first: an entry still waiting to
    // be submitted would be submitted later, into memory this call is
    // about to release. What the kernel refuses comes back to the pump,
    // where the next sweep drops this file's share.
    submit_or_lost(f);
    engine_.cancel_file(f.part_, f.inflight_);
    for (;;) {
      sweep(f);
      if (f.inflight_ == 0) {
        return;
      }
      const event e = engine_.template next<transfer>(true);
      if (e.what == event::kind::progress) {
        continue;
      }
      if (e.what == event::kind::wake) {
        woken_ = true;  // remember it; a later poll() still owes it
        continue;
      }
      if (e.what != event::kind::completion) {
        lost(" of a closing file", f.inflight_, e.ec);
      }
      transfer& op = *e.op;
      op.engine_held = false;
      if (op.owner_file == &f) {
        op.device_end();
        engine_.release(op);
        in_flight_--;
        f.inflight_--;  // reaped, not reported
        continue;
      }
      if (settle(op, e.ec, e.moved) == settled::requeued) {
        submit_or_lost(f);
      }
    }
  }

 private:
  enum class settled { reported, requeued, parked };

  // What every transfer needs from the context, whatever its direction.
  void arm(transfer& op, file& f) noexcept {
    op.owner_file = &f;
    op.next_deferred = nullptr;
    op.engine_held = false;
    in_flight_++;
    f.inflight_++;
  }

  // The rest of the transfer, from op.filled: on the kernel queue where
  // the engine takes it, otherwise to a lane.
  bool place(transfer& op) noexcept {
    op.engine_held = engine_.place(op);
    used_engine_ = used_engine_ || op.engine_held;
    if (!op.engine_held) {
      pump_.push(op);
    }
    return op.engine_held;
  }

  [[nodiscard]] auto to_pump() noexcept {
    return [this](transfer& op) noexcept {
      op.engine_held = false;
      pump_.push(op);
    };
  }

  // What one completion of a piece means: its callback, or the rest of a
  // short transfer placed again. A short piece with nothing moved ended
  // the file where the engine was told it would not.
  [[nodiscard]] settled settle(transfer& op, std::error_code ec,
                               std::size_t moved) noexcept {
    op.device_end();
    if (ec) {
      complete(op, ec);
      return settled::reported;
    }
    if (moved == 0) {
      complete(op, std::error_code(EIO, std::generic_category()));
      return settled::reported;
    }
    op.filled += moved;
    if (op.filled < op.length()) {
      return place(op) ? settled::requeued : settled::parked;
    }
    complete(op, {});
    return settled::reported;
  }

  // The submission boundary: everything built goes to the kernel, and a
  // failure the engine latched on the way comes back. Nothing waits on the
  // kernel queue before this has run since the last placement, because a
  // wait does not submit and would sleep on entries the kernel never had.
  void submit_or_throw() {
    flush();
    if (const std::error_code ec = engine_.take_failure()) {
      throw std::system_error(ec, std::string(engine_.mechanism()));
    }
  }
  void submit_or_lost(file& f) noexcept {
    flush();
    if (const std::error_code ec = engine_.take_failure()) {
      lost(" of a closing file", f.inflight_, ec);
    }
  }

  // One look at the kernel queue, settled. Returns whether it placed a
  // remainder, which the caller submits with the others before anything
  // waits.
  [[nodiscard]] bool take(const event& e) {
    switch (e.what) {
      case event::kind::empty:
      case event::kind::progress:
        return false;
      case event::kind::wake:
        woken_ = true;
        return false;
      case event::kind::failed:
        throw std::system_error(e.ec, std::string(engine_.mechanism()));
      case event::kind::completion:
        break;
    }
    e.op->engine_held = false;
    return settle(*e.op, e.ec, e.moved) == settled::requeued;
  }

  // Takes everything the kernel queue has ready, then submits the
  // remainders that produced in one flush. With `block`, while nothing was
  // reported and no wake arrived, it waits for one more event and goes
  // round again -- unless the pump holds work only this thread can do,
  // because no lane runs to take it.
  void reap(bool block, std::size_t before) {
    for (;;) {
      bool requeued = false;
      for (;;) {
        const event e = engine_.template next<transfer>(false);
        if (e.what == event::kind::empty) {
          break;
        }
        requeued = take(e) || requeued;
      }
      if (requeued) {
        submit_or_throw();
      }
      if (!block || reported_ != before || woken_ || pump_.needs_poller()) {
        return;
      }
      if (take(engine_.template next<transfer>(true))) {
        submit_or_throw();
      }
    }
  }

  // A wake is reported by the engine's next(), and remembered here
  // wherever it was seen -- an ordinary reap, or a closing file's or a
  // drain's wait -- until a poll() takes it.
  [[nodiscard]] bool take_wake() noexcept {
    return std::exchange(woken_, false);
  }

  // Every callback runs from here, which is what poll() counts: a
  // remainder moved to the pump is still owed, and is not one.
  void complete(transfer& op, std::error_code ec) noexcept {
    device_.add_if_served(op.device_total_ns());
    engine_.release(op);
    in_flight_ -= std::min<std::size_t>(1, in_flight_);
    op.owner_file->inflight_ -=
        std::min<std::size_t>(1, op.owner_file->inflight_);
    ++reported_;
    op.done(&op, ec);
  }

  // One transfer, on a lane. Touches the operation and the file it names,
  // and nothing else of this context, so served_synchronously_ is counted
  // where the completion is run.
  static std::error_code perform_deferred(transfer& op) noexcept {
    return Engine::perform(op);
  }

  void serve_deferred() noexcept {
    pump_.report_finished([this](transfer& op) noexcept {
      const std::error_code ec = op.result();
      if (!ec) {
        served_synchronously_++;
      }
      complete(op, ec);
    });
  }

  // This file's share of what never reached the kernel and of the pump:
  // queued work is dropped, a lane holding one of its transfers is waited
  // out, and what the lanes finished is dropped unreported.
  void sweep(file& f) noexcept {
    const auto mine = [&f](const transfer& op) noexcept {
      return op.owner_file == &f;
    };
    std::size_t dropped = engine_.template drop_unsubmitted<transfer>(mine);
    dropped += pump_.remove_queued_if(mine);
    pump_.quiesce();
    dropped += pump_.remove_done_if(mine);
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
  }

  // Requests in the kernel, a queue that cannot be waited on, and buffers
  // about to be reused: nothing can make the kernel let go, and returning
  // would let a transfer land in memory the caller has given back.
  [[noreturn]] void lost(const char* whose, std::size_t owed,
                                std::error_code ec) noexcept {
    std::fprintf(stderr,
                 "blake3pp: %.*s failed while %zu transfer(s)%s were still "
                 "owed (%s); their buffers cannot be released\n",
                 static_cast<int>(engine_.mechanism().size()),
                 engine_.mechanism().data(), owed, whose, ec.message().c_str());
    std::abort();
  }

  void adopt(file& f) noexcept {
    f.next_ = files_;
    files_ = &f;
  }
  void unlink(file& f) noexcept {
    for (file** at = &files_; *at != nullptr; at = &(*at)->next_) {
      if (*at == &f) {
        *at = f.next_;
        f.next_ = nullptr;
        return;
      }
    }
  }

  // Declared first, destroyed last: the kernel queue and the wake the
  // lanes signal outlive the pump's join.
  Engine engine_;
  file* files_ = nullptr;  // every open file, for the drain
  std::size_t in_flight_ = 0;
  std::size_t served_synchronously_ = 0;
  std::size_t reported_ = 0;  // callbacks run; poll() returns its growth
  device_time device_{};
  bool used_engine_ = false;  // any piece of this run reached the kernel
  bool woken_ = false;        // a wake an engine's next() delivered
  // The lanes that serve what the kernel will not take. Declared last, so
  // the destructor's join runs while the engine its wake() reaches is
  // still alive. Do not move this up.
  deferred_pump<transfer> pump_;
};

}  // namespace blake3pp::detail::io_impl
