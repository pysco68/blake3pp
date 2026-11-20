#pragma once

// The write side of the file pipeline: windows generated on a pool and
// written by the driver, on the caller's thread.
//
// One window in flight is one sender chain,
//
//     schedule(sched)
//       | then(fill)                // the caller's generator, on the pool
//       | continues_on(io.scheduler())
//       | let_value(io.write(win, off))
//       | then(release)
//
// which is the read chain read backwards, and it runs on the same
// driver_loop with the same three rules: callbacks only inside poll(),
// nothing completes inside a start(), and after a run-queue push the
// publisher touches only the loop.
//
// What it does NOT have is a reducer. Writes are order-free in a way
// reads are not: a window's bytes depend on its offset and nothing else,
// so there is no tree to rebuild and no last window to keep for the end.
// A write run shares the loop, the plan, the stop source, the error latch
// and quiesce with a read run; the reducer bookkeeping is absent.
//
// Internal to the library. Names an execution provider.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <blake3pp/detail/contract.hpp>
#include <blake3pp/detail/current_cpu.hpp>
#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/driver_loop.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/io_geometry.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp::detail {

// What a write run needs beyond the plan.
struct write_options {
  std::size_t window_bytes = default_write_window_bytes;
  // Resolved by the caller (resolve_write_queue_depth); a zero here is
  // clamped to the minimum like any other request.
  unsigned queue_depth = sequential_write_queue_depth;
  // Where to record one window_record per window, with the write side's
  // meaning of the stamps (trace.hpp); nullptr records nothing.
  trace_buffer* trace = nullptr;
  // Fills the scheduler can run at once, which is the generator's own
  // parallelism. One window is one fill task, so below this depth the
  // pool has threads with nothing to fill and the device's idle time is
  // the depth's own doing rather than the device waiting on the
  // generator; see note_generator_wait(). Zero means unknown, and the
  // depth is then taken as the parallelism, which is what it is
  // wherever the caller let resolve_write_queue_depth() pick it.
  unsigned generator_threads = 0;
  ex::inplace_stop_token stop_token{};
};

// Several write chains in flight over one file, all on the caller's
// thread.
//
// A window is a buffer the generator fills and the driver drains. The
// scope owns as many as the plan allows and hands each to one chain at a
// time; a chain may only start while a buffer is free and fewer than the
// plan's writes are in flight.
template <file_driver D, class Scheduler, class Fill>
class write_scope
    : public pipeline_scope_base<write_scope<D, Scheduler, Fill>, D> {
  // The base drives this scope's loop and sweep, and holds the state the
  // read scope shares.
  friend class pipeline_scope_base<write_scope, D>;

  using base = pipeline_scope_base<write_scope, D>;
  using base::ec_;
  using base::eptr_;
  using base::in_flight_;
  using base::transfers_;
  using base::transfers_owed_;
  using base::latch_error;
  using base::latch_exception;
  using base::quiesce;
  using base::request_stop;
  using base::status_;
  using base::stop_requested;
  using base::stop_source_;

 public:
  write_scope(D& drv, typename D::file& file, Scheduler sched, Fill fill,
              std::uint64_t total, const write_options& opts)
      : loop_(drv),
        file_(file),
        sched_(std::move(sched)),
        fill_(std::move(fill)),
        total_(total),
        window_bytes_(rounded_window_bytes(opts.window_bytes)),
        count_(plan_windows(total, window_bytes_, opts.queue_depth)),
        in_flight_cap_(
            std::min(clamp_queue_depth(opts.queue_depth), count_)),
        generator_threads_(opts.generator_threads != 0
                               ? opts.generator_threads
                               : in_flight_cap_),
        trace_(opts.trace),
        opts_stop_token_(opts.stop_token),
        windows_(std::make_unique<window[]>(count_)) {
    drv.reserve(count_, window_bytes_);
    for (unsigned i = count_; i > 0; --i) {
      window& w = windows_[i - 1];
      w.slot = i - 1;
      push_free(&w);
    }
  }

  write_scope(const write_scope&) = delete;
  write_scope& operator=(const write_scope&) = delete;

  // The windows, the operations naming them and the driver's arena all
  // outlive this scope by one frame at most, so nothing of this run may
  // still be running here: a fill writing into a freed window is the
  // failure this catches. run() ends with nothing outstanding or winds
  // down through quiesce(), which returns only once every chain has
  // completed or been reclaimed from the driver -- reclaimed ones stay
  // counted, hence the comparison rather than a zero.
  ~write_scope() {
    assert(in_flight_ == transfers_owed_ &&
           "a window was still on the pool when its write scope went");
  }

  // Drives the whole file on this thread.
  //
  // Ends exactly once, and only after everything in flight has landed:
  // with the file exactly `total` bytes, with the first failure any
  // window reported, or with operation_canceled if the run was stopped.
  // On a failure or a stop the file is NOT truncated, and what lies past
  // the last landed window is unspecified -- the caller asked for bytes
  // that were never produced, and inventing a length would hide that.
  void run() {
    if (total_ == 0) {
      file_.set_size(0);
      return;
    }
    std::optional<outer_stop_callback> watch;
    if (opts_stop_token_.stop_possible()) {
      if (opts_stop_token_.stop_requested()) {
        request_stop();
        throw_if_stopped();
        return;
      }
      watch.emplace(opts_stop_token_,
                    request_scope_stop{&stop_source_, &loop_.driver()});
    }

    using cell_type = ex::connect_result_t<
        decltype(std::declval<write_scope&>().window_chain(
            std::declval<window&>())),
        scope_receiver>;
    struct connect_one {
      write_scope* scope;
      window* w;
      operator cell_type() const {
        return ex::connect(scope->window_chain(*w), scope_receiver{scope, w});
      }
    };
    auto cells = std::make_unique<std::optional<cell_type>[]>(count_);

    try {
      run_windows(cells, connect_one{});
    } catch (...) {
      quiesce();
      throw;
    }
    assert(in_flight_ == 0 && "run_until returned with a window in flight");
    watch.reset();

    if (eptr_) {
      std::rethrow_exception(eptr_);
    }
    if (ec_) {
      throw std::system_error(ec_, "blake3pp: writing a window");
    }
    throw_if_stopped();
    // Every byte asked for has landed; trim a preallocation to match.
    file_.set_size(total_);
  }

  // What the run did, in the same fields a read run fills.
  void report(driver_stats& d) const noexcept {
    d.queue_depth_final = in_flight_cap_;
    d.queue_depth_peak =
        std::max<std::uint64_t>(d.queue_depth_peak, in_flight_cap_);
    d.agent_busy += std::chrono::nanoseconds{
        static_cast<std::chrono::nanoseconds::rep>(
            fill_busy_ns_.load(std::memory_order_relaxed))};
    transfers_.report(d, loop_.driver());
  }

 private:
  struct window {
    std::span<std::byte> buffer;
    io_op op{};
    // When the write was handed to the driver, on steady_clock. Kept
    // whether or not a trace buffer was given: the device counters are
    // the landing minus this.
    trace_clock::time_point t_write_begin{};
    window* next_free = nullptr;
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
    unsigned slot = 0;
    bool writing = false;
    bool cancel_asked = false;
    window_record* rec = nullptr;
  };

  using scope_receiver = typename base::template receiver<window>;
  static_assert(std::copy_constructible<scope_receiver>);

  using typename base::outer_stop_callback;
  using typename base::request_scope_stop;

  // How many window buffers the run uses: what the queue depth asks for,
  // never more than the file has windows.
  [[nodiscard]] static unsigned plan_windows(std::uint64_t total,
                                             std::size_t window,
                                             unsigned qd) noexcept {
    const unsigned want = clamp_queue_depth(qd);
    const std::uint64_t have = (total + window - 1) / window;
    return have < want ? static_cast<unsigned>(have) : want;
  }

  // The chain, read backwards from the read side's. Untagged, unlike the
  // read side's builders, for the reason window_scope.hpp gives at its
  // compress stages: the body builds the same non-polymorphic layout in
  // every unit, so the one copy the linker keeps serves them all.
  [[nodiscard]] auto window_chain(window& w) {
    return ex::then(
        ex::let_value(
            declared(ex::continues_on(
                ex::then(ex::schedule(sched_),
                         [this, &w] { fill_window(w); }),
                loop_.scheduler())),
            [this, &w] {
              w.t_write_begin = steady_now();
              return loop_.write(file_, w.offset,
                                 std::span<const std::byte>(w.buffer.data(),
                                                            w.bytes),
                                 w.op, transfers_owed_);
            }),
        [this, &w]() noexcept { landed(w); });
  }

  // The generator may throw, on a pool thread, inside a window already
  // counted in flight: the chain carries it from there like any other
  // stage's exception. The window's own completion is set_error with the
  // exception_ptr, the scope latches it, stops the run, gives the window
  // back, and run() rethrows it to whoever called write_file(). The file is
  // then left as far as it got, which is what an I/O failure leaves too.
  //
  // Nothing is submitted for this window before the fill, so a throw owes
  // the driver nothing.
  void fill_window(window& w) {
    // On an agent: the fill's two stamps, and with agent recording on,
    // one agent record for the fill -- one part, on this CPU.
    //
    // The two stamps are taken for a trace buffer with no per-window
    // storage too, because the generator's busy share is what says
    // whether the pool or the device is the constraint and a caller
    // may want that without one record per window. Two clock reads and
    // one relaxed add per window; nothing at all without a buffer.
    if (trace_ == nullptr) {
      fill_(w.buffer.first(w.bytes), w.offset);
      return;
    }
    const trace_clock::time_point t_begin = trace_->now();
    if (w.rec != nullptr) {
      w.rec->t_ready = t_begin;
    }
    fill_(w.buffer.first(w.bytes), w.offset);
    const trace_clock::time_point t_end = trace_->now();
    const std::chrono::nanoseconds busy = t_end - t_begin;
    fill_busy_ns_.fetch_add(static_cast<std::uint64_t>(busy.count()),
                            std::memory_order_relaxed);
    if (w.rec != nullptr) {
      w.rec->t_joined = t_end;
      w.rec->agents_active = 1;
      w.rec->agent_busy = busy;
      if (agent_record* const a = trace_->claim_agent(); a != nullptr) {
        a->window = w.rec->index;
        a->file = 0;
        a->t_begin = w.rec->t_ready;
        a->busy = busy;
        a->parts = 1;
        a->min_part = a->max_part = busy;
        a->cpu = current_cpu();
      }
    }
  }

  void landed(window& w) noexcept {
    w.writing = false;
    const trace_clock::time_point t =
        transfers_.land(loop_.driver(), w.op, w.t_write_begin, w.bytes);
    if (w.rec != nullptr) {
      w.rec->t_absorbed = t;
    }
  }

  template <class Cells, class ConnectOne>
  void run_windows(Cells& cells, ConnectOne) {
    loop_.run_until(status_, [&] {
      if (stop_requested()) {
        sweep_cancels();
      }
      while (!stop_requested() && in_flight_ < in_flight_cap_ &&
             free_ != nullptr && next_offset_ < total_) {
        window* const w = free_;
        free_ = w->next_free;
        // Built, then counted: anything that throws before the start --
        // the geometry check below, a provider's operation state -- puts
        // the window back on the free list rather than leaving a count
        // this run can never work off.
        try {
          take_next(*w);
          cells[w->slot].emplace(ConnectOne{this, w});
        } catch (...) {
          w->op.live = false;
          w->next_free = free_;
          free_ = w;
          throw;
        }
        ++in_flight_;
        w->writing = true;
        status_.outstanding = in_flight_;
        ex::start(*cells[w->slot]);
      }
      note_generator_wait();
      settle();
    }, [this] { this->report_stall(); }, trace_);
  }

  // Not noexcept: the geometry check below is one the chain's own error
  // channel carries, and admission runs before the window is counted.
  void take_next(window& w) {
    w.cancel_asked = false;
    w.offset = next_offset_;
    w.bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(window_bytes_, total_ - next_offset_));
    w.rec = trace_ != nullptr ? trace_->claim_window() : nullptr;
    if (w.rec != nullptr) {
      w.rec->index = w.offset / window_bytes_;
      w.rec->bytes = w.bytes;
      w.rec->slot = w.slot;
      w.rec->flags = w.offset + w.bytes == total_ ? window_record::flag_last : 0;
      w.rec->t_wait_begin = trace_->now();
    }
    const window_storage storage =
        loop_.driver().window_storage_for(file_, w.slot, w.offset, w.bytes);
    // A write needs somewhere to generate INTO, so a driver that hands out
    // views rather than buffers cannot serve this scope. Checked in every
    // build: past it the window's buffer is an empty span and the chain
    // hands the generator {nullptr, w.bytes} to fill.
    require(!storage.read_into.empty(),
            "the write pipeline needs a driver that owns window buffers");
    w.buffer = storage.read_into;
    next_offset_ += w.bytes;
  }

  // The write side's one stall more depth cannot help: the driver has
  // no write outstanding while the run still owes bytes, so every
  // window the run holds is being generated and the device is idle
  // waiting for one.
  //
  // This is the opposite event from the read side's, not the same one.
  // There an empty window pool means the hash is behind; here an empty
  // pool is the steady state, because generation outruns any device.
  //
  // It is counted only at and above the generator's own parallelism.
  // One window is one fill task, so the depth is how many fills run at
  // once as well as how many writes do: below the pool's width the
  // device is idle because the depth left threads with nothing to
  // fill, which is the depth's own doing and not the generator failing
  // to keep up. driver_stats::starved is the time lost for a reason
  // more depth cannot fix, so that stretch does not belong in it. At
  // and above the width the generator is already as parallel as the
  // pool can make it and the signal means what it says.
  void note_generator_wait() noexcept {
    transfers_.starved_now(in_flight_cap_ >= generator_threads_ &&
                       loop_.driver().in_flight() == 0 &&
                       !stop_requested() && next_offset_ < total_);
  }

  [[noreturn]] void report_stall() const {
    throw std::logic_error(
        "blake3pp: the write pipeline cannot start a window and nothing is "
        "in flight");
  }

  void sweep_cancels() noexcept {
    for (unsigned i = 0; i < count_; ++i) {
      window& w = windows_[i];
      // Only a window whose write the driver actually has. One that is
      // admitted but still being filled on the pool has nothing to
      // cancel, and latching it here would also stop the sweep asking
      // once its write really is in flight.
      if (w.writing && w.op.live && !w.cancel_asked) {
        w.cancel_asked = true;
        loop_.driver().cancel(w.op);
      }
    }
  }

  void finish_window(window& w) noexcept {
    w.writing = false;
    --in_flight_;
    if (w.rec != nullptr) {
      w.rec->t_released = trace_->now();
      w.rec = nullptr;
    }
    push_free(&w);
    settle();
  }

  void settle() noexcept {
    status_.outstanding = in_flight_;
    if (in_flight_ > 0) {
      return;
    }
    if (stop_requested() || next_offset_ >= total_) {
      status_.done = true;
    }
  }

  void throw_if_stopped() const {
    base::throw_if_stopped("blake3pp: the write pipeline was stopped");
  }

  // The loop the base's quiesce drives, which this scope owns.
  [[nodiscard]] driver_loop<D>& loop() noexcept { return loop_; }
  // What the base's teardown reclaims transfers from: this run's file,
  // never the whole driver, which may be serving other pipelines.
  [[nodiscard]] typename D::file& file() noexcept { return file_; }

  void push_free(window* w) noexcept {
    w->next_free = free_;
    free_ = w;
  }

  driver_loop<D> loop_;
  typename D::file& file_;
  Scheduler sched_;
  Fill fill_;
  std::uint64_t total_;
  std::size_t window_bytes_;
  unsigned count_;
  unsigned in_flight_cap_;
  // Fills the scheduler runs at once (write_options::generator_threads),
  // or the depth where the caller did not say.
  unsigned generator_threads_;
  trace_buffer* trace_ = nullptr;
  ex::inplace_stop_token opts_stop_token_;
  std::unique_ptr<window[]> windows_;


  window* free_ = nullptr;
  std::uint64_t next_offset_ = 0;
  // The generator's summed fill time over the run. Written by the
  // agent that filled a window, read by the driver after everything
  // has joined, so it is atomic where the other counters are not.
  std::atomic<std::uint64_t> fill_busy_ns_{0};
};

// Generates one file on this thread, fanning the fills out over sched.
template <file_driver D, class Scheduler, class Fill>
void run_write_pipeline(D& drv, typename D::file& file, Scheduler&& sched,
                        Fill&& fill, std::uint64_t total,
                        const write_options& opts) {
  // decay, not remove_cvref: a generator passed as a plain function
  // deduces to a function TYPE, which cannot be a member.
  write_scope<D, std::remove_cvref_t<Scheduler>, std::decay_t<Fill>>
      scope(drv, file, std::forward<Scheduler>(sched),
            std::forward<Fill>(fill), total, opts);
  scope.run();
  if (opts.trace != nullptr) {
    scope.report(opts.trace->driver());
  }
}

}  // namespace blake3pp::detail
