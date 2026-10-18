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

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <blake3pp/detail/compress.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/file_writer.hpp>

namespace blake3pp::detail {

// What a write run needs beyond the plan.
struct write_options {
  std::size_t window_bytes = default_write_window_bytes;
  unsigned queue_depth = default_write_queue_depth;
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
  using base::dropped_failures_;
  using base::ec_;
  using base::eptr_;
  using base::in_flight_;
  using base::transfers_owed_;
  using base::latch_error;
  using base::latch_exception;
  using base::quiesce;
  using base::request_stop;
  using base::status_;
  using base::stop_requested;
  using base::stop_source_;
  using base::stopped_;

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
        in_flight_cap_(clamp_queue_depth(opts.queue_depth)),
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
        stopped_ = true;
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

 private:
  struct window {
    std::span<std::byte> buffer;
    io_op op{};
    window* next_free = nullptr;
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
    unsigned slot = 0;
    bool writing = false;
    bool cancel_asked = false;
  };

  struct scope_env {
    ex::inplace_stop_token token;
    [[nodiscard]] ex::inplace_stop_token query(
        ex::get_stop_token_t) const noexcept {
      return token;
    }
  };

  struct scope_receiver {
    using receiver_concept = ex_compat::receiver_tag;
    scope_receiver(write_scope* s, window* win) noexcept : scope(s), w(win) {}
    write_scope* scope;
    window* w;

    [[nodiscard]] scope_env get_env() const noexcept {
      return {scope->stop_source_.get_token()};
    }
    void set_value() && noexcept { scope->on_done(*w); }
    void set_error(std::error_code ec) && noexcept { scope->on_error(*w, ec); }
    void set_error(std::exception_ptr e) && noexcept {
      scope->on_exception(*w, std::move(e));
    }
    void set_stopped() && noexcept { scope->on_stopped(*w); }
  };

  static_assert(std::copy_constructible<scope_receiver>);

  using request_scope_stop = blake3pp::detail::request_scope_stop<D>;
  using outer_stop_callback =
      ex::stop_callback_for_t<ex::inplace_stop_token, request_scope_stop>;

  // How many window buffers the run uses: what the queue depth asks for,
  // never more than the file has windows.
  [[nodiscard]] static unsigned plan_windows(std::uint64_t total,
                                             std::size_t window,
                                             unsigned qd) noexcept {
    const unsigned want = clamp_queue_depth(qd);
    const std::uint64_t have = (total + window - 1) / window;
    return have < want ? static_cast<unsigned>(have) : want;
  }

  // The chain, read backwards from the read side's.
  [[nodiscard]] auto window_chain(window& w) {
    return ex::then(
        ex::let_value(
            declared(ex::continues_on(
                ex::then(ex::schedule(sched_),
                         [this, &w]() noexcept { fill_window(w); }),
                loop_.scheduler())),
            [this, &w] {
              return loop_.write(file_, w.offset,
                                 std::span<const std::byte>(w.buffer.data(),
                                                            w.bytes),
                                 w.op, transfers_owed_);
            }),
        [this, &w]() noexcept { landed(w); });
  }

  // Marked noexcept, which is the caller's contract too: an exception
  // escaping the generator terminates rather than unwinding through a
  // stage whose window is already counted in flight. write_file's
  // documentation says so, and the static_assert makes a generator that
  // can throw a compile error rather than a crash under load -- the
  // sequential forms of write_file() take one that may throw, and this is
  // the path that cannot.
  void fill_window(window& w) noexcept {
    static_assert(
        std::is_nothrow_invocable_v<Fill&, std::span<std::byte>, std::uint64_t>,
        "the scheduler-taking write_file() needs a noexcept generator: it "
        "runs on a pool thread inside a window already counted in flight, "
        "where an exception has nowhere to go");
    fill_(w.buffer.first(w.bytes), w.offset);
  }

  void landed(window& w) noexcept { w.writing = false; }

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
      settle();
    }, [this] { this->report_stall(); });
  }

  // Not noexcept: the geometry check below is one the chain's own error
  // channel carries, and admission runs before the window is counted.
  void take_next(window& w) {
    w.cancel_asked = false;
    w.offset = next_offset_;
    w.bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(window_bytes_, total_ - next_offset_));
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

  void on_done(window& w) noexcept { finish_window(w); }

  void on_error(window& w, std::error_code ec) noexcept {
    latch_error(ec);
    request_stop();
    finish_window(w);
  }

  void on_exception(window& w, std::exception_ptr e) noexcept {
    latch_exception(std::move(e));
    request_stop();
    finish_window(w);
  }

  void on_stopped(window& w) noexcept {
    stopped_ = true;
    request_stop();
    finish_window(w);
  }

  void finish_window(window& w) noexcept {
    w.writing = false;
    --in_flight_;
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
  ex::inplace_stop_token opts_stop_token_;
  std::unique_ptr<window[]> windows_;


  window* free_ = nullptr;
  std::uint64_t next_offset_ = 0;

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
}

}  // namespace blake3pp::detail
