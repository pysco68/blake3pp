#pragma once

/// @file
/// EXPERIMENTAL. File hashing as senders, over a context the caller
/// drives.
///
/// The blocking entry points in parallel_io.hpp run one file and own
/// everything they need for it: a driver, an arena of windows, and the
/// thread that calls them. That is the right shape for one file and the
/// wrong one for a thousand, where the arena is allocated and faulted in
/// once per call and no two files ever overlap.
///
/// Here the storage moves up. An io_context owns the driver, the window
/// arena and the run queue; a sender borrows windows from it for as long
/// as one file takes, and several senders may be in flight at once. One
/// thread runs the context, and every operation completes on it; an
/// operation may be started from any thread, and its file is opened and
/// its reads begin on the running thread.
///
/// ```cpp
/// blake3pp::async::io_context ctx;
/// exec::static_thread_pool pool{std::thread::hardware_concurrency()};
/// std::thread runner([&] { ctx.run(); });  // until request_stop()
///
/// auto work = stdexec::when_all(
///     blake3pp::async::async_hash_file(ctx, "a.bin", pool.get_scheduler()),
///     blake3pp::async::async_hash_file(ctx, "b.bin", pool.get_scheduler()));
/// auto [a, b] = stdexec::sync_wait(std::move(work)).value();
///
/// ctx.request_stop();
/// runner.join();
/// ```
///
/// A thread with nothing else to do can drive the context itself:
/// start the operations, then run_until_idle() returns once they have
/// completed.
///
/// What is experimental about it: the error channels and the names of
/// the context options. The run model and the lifetime rules below are
/// the parts meant to stay.
///
/// LIFETIMES. A started operation must not outlive its context, its
/// scheduler or (for async_update_file) its hasher and path, and a
/// context with a started, uncompleted operation must not be destroyed.
/// The hasher an async_update_file names is the operation's alone until
/// it completes.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/ex.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/io_driver.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>

namespace blake3pp::async {

/// How much storage a context takes and how it opens files.
///
/// The geometry is the context's, not the call's: the arena is one
/// allocation shared by every operation, so the window size and the
/// number of windows are settled when the context is built. A single
/// operation may hold at most `queue_depth` of them at once, which is
/// what keeps one large file from starving the rest.
struct io_context_options {
  /// Bytes per window, 8 MiB by default; rounded down to a power-of-2
  /// multiple of chunk_size, minimum 64 KiB.
  std::size_t window_bytes = detail::default_window_bytes;
  /// Windows one operation may hold at once, four by default; clamped to
  /// [2, 256].
  unsigned queue_depth = detail::default_queue_depth;
  /// Windows in the shared arena, at window_bytes each. Zero means what
  /// the plan asks for, which at the defaults is 16 windows and so a
  /// 128 MiB arena -- several operations' worth already. Clamped to the
  /// same ceiling as queue_depth, because every window costs the same
  /// window_bytes whether it was asked for as a count or as a budget.
  unsigned windows = 0;
  /// The arena as a byte budget instead: when non-zero, the window count
  /// is this divided by the window size, at least queue_depth and under
  /// the same ceiling, and `windows` is ignored. The budget is what a caller
  /// knows; how many windows it buys depends on the window size.
  std::size_t arena_bytes = 0;
  /// Bypass the page cache where the platform supports it, per file.
  /// A file whose window grid is unaligned is opened buffered whatever
  /// this says, because every window after an unaligned one starts
  /// unaligned too.
  bool direct_io = true;
  /// Issue reads on the kernel's I/O worker threads; see
  /// file_io_options.
  bool offload_submit = true;
  /// Windows: prefer the I/O ring (Windows 11, `ioringapi.h`) over the
  /// completion port where the machine has one. Off selects the port,
  /// which is also what a machine without a ring gets; the engaged rung
  /// is reported after the fact and names which one ran.
  bool ioring = true;
};

namespace detail {
using blake3pp::detail::io_driver;

/// One started operation, as the context's loop sees it: something to
/// offer a round to, that eventually says it is finished.
///
/// Every member runs on the context's thread, inside the loop.
struct pipeline_node {
  pipeline_node* next = nullptr;
  /// Starts whatever this operation can start before the next flush.
  virtual void round() noexcept = 0;
  /// Whether this operation has nothing left to do.
  [[nodiscard]] virtual bool finished() const noexcept = 0;
  /// Windows this operation still has in flight, which is what tells
  /// the loop whether sleeping could ever be woken.
  [[nodiscard]] virtual unsigned outstanding() const noexcept = 0;
  /// Completes the receiver. Called once, after the node has been
  /// unlinked, and may destroy this object.
  virtual void report() noexcept = 0;
  /// Asks this operation to stop; the effect arrives in a later round.
  virtual void request_stop() noexcept = 0;

 protected:
  ~pipeline_node() = default;
};
}  // namespace detail

/// A driver, an arena of windows and the one thread that runs them.
///
/// One thread may call run() or run_until_idle(); every other thread may
/// call request_stop(), and may start operations on the context: a
/// started operation joins the loop's list through an atomic push, and
/// everything it does -- the open, the plan, the reads -- happens on the
/// running thread from its first round. Operations complete on the
/// running thread, inside the run call, which is also the only place a
/// receiver of one of these senders is ever invoked.
///
/// @tparam Budget What a window's part table may take. It is the
///                context's rather than the call's because the context
///                owns the tables; see stack_budget.
template <stack_budget Budget = default_stack_budget>
class basic_io_context {
 public:
  /// What continues_on() takes to come back to this context.
  using scheduler_type =
      blake3pp::detail::driver_scheduler<blake3pp::detail::io_driver>;

  explicit basic_io_context(const io_context_options& opts = {})
      : opts_(opts),
        plan_(blake3pp::detail::plan_pipeline(0, opts.window_bytes,
                                              opts.queue_depth, opts.direct_io,
                                              /*file_bytes=*/0)),
        // Every window holds at most one read, so the pool's size is
        // also the most the driver can ever have outstanding.
        drv_({.async = true,
              .offload_submit = opts.offload_submit,
              .ioring = opts.ioring},
             pool_windows(opts, plan_)),
        host_(drv_, pool_windows(opts, plan_), plan_.window_bytes) {}

  basic_io_context(const basic_io_context&) = delete;
  basic_io_context& operator=(const basic_io_context&) = delete;

  ~basic_io_context() {
    // P2300's rule: an operation state may not be destroyed between
    // start() and its completion, and the context is what would have
    // completed it.
    assert(head_ == nullptr && pending_.load(std::memory_order_acquire) == nullptr &&
           "an io_context was destroyed with an operation started and not "
           "completed");
  }

  /// Where a stage comes back to when it has to touch the driver again.
  [[nodiscard]] scheduler_type scheduler() noexcept {
    return host_.loop().scheduler();
  }

  /// Runs until request_stop() has been called and nothing is owed.
  ///
  /// With nothing started it blocks until request_stop(), which is what
  /// a thread dedicated to the context wants; run_until_idle() is the
  /// call for a thread that has other work.
  void run() { drive(/*until_idle=*/false); }

  /// Runs until no operation is in flight, then returns. A context with
  /// nothing started returns immediately.
  void run_until_idle() { drive(/*until_idle=*/true); }

  /// Runs until `done` reads true, whichever thread sets it; other
  /// operations may still be in flight when it returns. What sync_wait()
  /// drives. The flag is atomic because a composed sender's last stage
  /// may complete off this thread, and whoever sets it must then wake
  /// this context -- which is what sync_wait()'s receiver does.
  void run_until(const std::atomic<bool>& done) {
    drive(/*until_idle=*/false, &done);
  }

  /// How many windows the arena holds.
  [[nodiscard]] unsigned windows() const noexcept {
    return pool_windows(opts_, plan_);
  }

  /// Asks every started operation to stop and wakes the running thread.
  /// The only member another thread may call.
  void request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    drv_.wake();
  }

  /// Whether a stop has been requested. Clears on the next run().
  [[nodiscard]] bool stop_requested() const noexcept {
    return stop_.load(std::memory_order_acquire);
  }

  /// @cond INTERNAL
  // sync_wait()'s spelling of run_until(): `start` runs once this thread
  // holds the context, so a claim that fails -- another thread is running
  // it -- throws before anything has been started on it.
  template <class Start>
  void run_until(const std::atomic<bool>& done, Start&& start) {
    drive(/*until_idle=*/false, &done, start);
  }

  // What a sender's operation state needs; not public API.
  [[nodiscard]] blake3pp::detail::pipeline_host<Budget,
                                                blake3pp::detail::io_driver>&
  host() noexcept {
    return host_;
  }
  [[nodiscard]] blake3pp::detail::io_driver& driver() noexcept { return drv_; }
  [[nodiscard]] const blake3pp::detail::pipeline_plan& plan() const noexcept {
    return plan_;
  }
  [[nodiscard]] const io_context_options& options() const noexcept {
    return opts_;
  }

  // Joins the loop's round-robin, from any thread. The node is the
  // caller's; it waits on the pending list until the running thread
  // adopts it at the top of a pass, and leaves the list when it reports.
  void link(detail::pipeline_node* n) noexcept {
    n->next = pending_.load(std::memory_order_relaxed);
    while (!pending_.compare_exchange_weak(n->next, n,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
    }
    // A node started from another operation's completion joins a loop
    // that is already running; one started before run() is picked up
    // when it begins. Either way the driver is nudged, because the loop
    // may be parked in poll().
    drv_.wake();
  }
  /// @endcond

 private:
  static unsigned pool_windows(
      const io_context_options& opts,
      const blake3pp::detail::pipeline_plan& plan) noexcept {
    const unsigned ceiling = blake3pp::detail::max_queue_depth;
    if (opts.arena_bytes != 0) {
      const std::size_t by_budget =
          opts.arena_bytes / std::max<std::size_t>(1, plan.window_bytes);
      return static_cast<unsigned>(std::clamp<std::size_t>(
          by_budget, std::max(1u, plan.reads_in_flight), ceiling));
    }
    // A count given outright is clamped like one derived from a budget:
    // every window is window_bytes of arena, so the two requests have the
    // same ceiling whichever way they are spelled.
    return opts.windows != 0 ? std::min(opts.windows, ceiling) : plan.windows;
  }

  struct no_start {
    void operator()() const noexcept {}
  };

  template <class Start = no_start>
  void drive(bool until_idle, const std::atomic<bool>* until = nullptr,
             Start&& start = {}) {
    // Reported in every build, like the pipeline's own stall report and for
    // the same reason: two threads inside one context is a shape a caller
    // configured, not a bug a sanitizer lane would find, and under NDEBUG
    // the second one would walk the operation list the first is mutating.
    // run() is not noexcept, so this one can be thrown.
    if (running_.exchange(true, std::memory_order_acq_rel)) {
      throw std::logic_error(
          "blake3pp: a second thread called run() on an io_context; only one "
          "may run it, and the others may call request_stop() and start "
          "operations");
    }
    // Whatever leaves this frame, the context is runnable again: an
    // exception out of the driver's poll leaves the started operations
    // linked and unfinished, and the next run() continues them.
    struct running_guard {
      std::atomic<bool>& flag;
      ~running_guard() { flag.store(false, std::memory_order_release); }
    } const guard{running_};
    start();
    blake3pp::detail::loop_status status{};
    host_.loop().run_until(
        status,
        [&] {
          // Operations started since the last pass, from any thread,
          // join the list here, on the running thread.
          for (detail::pipeline_node* n =
                   pending_.exchange(nullptr, std::memory_order_acquire);
               n != nullptr;) {
            detail::pipeline_node* const next = n->next;
            n->next = head_;
            head_ = n;
            ++live_;
            n = next;
          }
          if (stop_.load(std::memory_order_acquire)) {
            for (detail::pipeline_node* n = head_; n != nullptr; n = n->next) {
              n->request_stop();
            }
          }
          // One round each, then the finished ones are reported. A
          // report may start another operation, which is why the walk
          // and the reporting are separate passes: link() writes head_.
          detail::pipeline_node* done = nullptr;
          detail::pipeline_node** link = &head_;
          unsigned owed = 0;
          while (*link != nullptr) {
            detail::pipeline_node* const n = *link;
            n->round();
            if (n->finished()) {
              *link = n->next;
              --live_;
              n->next = done;
              done = n;
            } else {
              owed += n->outstanding();
              link = &n->next;
            }
          }
          while (done != nullptr) {
            detail::pipeline_node* const n = done;
            done = n->next;
            n->next = nullptr;
            n->report();  // may destroy n, and may start another
          }
          status.outstanding = owed;
          // A report may have started another operation; it is pending,
          // not live, so the list is consulted too.
          status.done =
              (until != nullptr && until->load(std::memory_order_acquire)) ||
              (live_ == 0 &&
               pending_.load(std::memory_order_acquire) == nullptr &&
               (until_idle || stop_.load(std::memory_order_acquire)));
        },
        [&] {
          // Nothing owed, nothing to run, and nothing the driver can
          // deliver. For a context that is not a misconfiguration: it is
          // a thread waiting for work or for a stop, so it sleeps.
          // run_until only calls this when it is about to block, and a
          // wake() from link() or request_stop() is what ends it.
        });
    // The stop is cleared only once there is nothing left for it to reach:
    // a sync_wait that returns on its own flag can leave siblings live, and
    // they, plus anything started while the stop stands, must still see it.
    if (live_ == 0 && pending_.load(std::memory_order_acquire) == nullptr) {
      stop_.store(false, std::memory_order_release);
    }
  }

  io_context_options opts_;
  blake3pp::detail::pipeline_plan plan_;
  blake3pp::detail::io_driver drv_;
  blake3pp::detail::pipeline_host<Budget, blake3pp::detail::io_driver> host_;
  detail::pipeline_node* head_ = nullptr;
  unsigned live_ = 0;
  // Started and not yet adopted by the loop: the one list another thread
  // writes.
  std::atomic<detail::pipeline_node*> pending_{nullptr};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
};

/// The context, with the default part-table budget.
using io_context = basic_io_context<default_stack_budget>;

namespace detail {

// What the receiver's stop does: request this operation's own source,
// which the scope is already watching -- its callback is what wakes the
// driver, so nothing here has to.
struct forward_stop {
  blake3pp::ex::inplace_stop_source* source;
  void operator()() const noexcept { source->request_stop(); }
};

// One file, streamed into a hasher the caller owns, as an operation
// state.
//
// The shape is the blocking pipeline's, split where a sender needs it:
// start() opens the file and joins the context's round-robin, the
// context offers it a round at a time, and the round that finishes it
// reports on the receiver instead of returning.
//
// Nothing happens inside start() but joining the context: start() may
// run on any thread, and the driver belongs to the running one. The
// open, the plan and the first reads are the first round's, and a
// failure there is latched and reported from the round after, which is
// the same rule every stage inside the pipeline already obeys.
// The cell store is a template parameter, not a member alias, for the
// reason RULE 6 in detail/ex_compat.hpp gives: it carries provider
// sender types, which have internal linkage, so naming it here keeps
// this operation state internal to its translation unit too. Without
// it, two units connecting the same sender with the same
// externally-linked receiver would agree on this class's name and
// disagree on its layout.
template <stack_budget Budget, class Sched, class Receiver, class Value,
          class Cells, tracer Tracer>
class update_file_op final : public pipeline_node {
 public:
  using operation_state_concept = blake3pp::detail::ex_compat::operation_state_tag;
  using scope_type =
      blake3pp::detail::window_scope<Tracer, Budget, io_driver, Sched>;
  using cells_type = Cells;

  // Over a hasher the caller owns: it must outlive the operation, and
  // nothing else may touch it while this runs.
  update_file_op(basic_io_context<Budget>& ctx, hasher& h,
                 std::filesystem::path path, Sched sched,
                 const file_io_options& opts, Tracer tracer,
                 std::uint32_t file_index, Receiver r)
      : ctx_(&ctx),
        h_(&h),
        path_(std::move(path)),
        sched_(std::move(sched)),
        opts_(opts),
        tracer_(tracer),
        file_index_(file_index),
        rcvr_(std::move(r)) {}

  // Over a hasher of its own, built from the call's options and
  // finalized into the value this completes with.
  update_file_op(basic_io_context<Budget>& ctx, std::filesystem::path path,
                 Sched sched, const hash_file_options& opts, Tracer tracer,
                 std::uint32_t file_index, Receiver r)
      : ctx_(&ctx),
        own_h_(blake3pp::detail::make_hasher(opts, blake3pp::detail::resolve(opts.a))),
        h_(&*own_h_),
        path_(std::move(path)),
        sched_(std::move(sched)),
        opts_(opts),
        tracer_(tracer),
        file_index_(file_index),
        rcvr_(std::move(r)) {}

  update_file_op(const update_file_op&) = delete;
  update_file_op& operator=(const update_file_op&) = delete;

  void start() noexcept {
    // The receiver's stop, bridged onto the source the scope watches.
    // Installed before anything is opened, so a token already stopped
    // takes the same path as one stopped mid-file.
    watch_receiver_stop();
    ctx_->link(this);
  }

  void round() noexcept override {
    if (!opened_) {
      opened_ = true;
      open();
    }
    if (!live_) {
      return;
    }
    try {
      scope_->admit_round(*cells_);
    } catch (...) {
      // The round threw, so the cells cannot be destroyed yet: a
      // window on the pool may be about to publish into one. Wait those
      // out first, which also returns the windows this run held, then
      // report from the next round.
      scope_->abandon();
      eptr_ = std::current_exception();
      live_ = false;
    }
  }

 private:
  // The first round's work, on the running thread.
  void open() noexcept {
    try {
      // The size decides the open mode and the window count, so it is
      // read before the open; a size the filesystem cannot give leaves
      // the plan to the context's geometry and the open reports why.
      std::error_code size_ec;
      const std::uintmax_t size = std::filesystem::file_size(path_, size_ec);
      const io_context_options& cfg = ctx_->options();
      plan_ = blake3pp::detail::plan_pipeline(
          h_->count(), cfg.window_bytes, cfg.queue_depth,
          cfg.direct_io && opts_.direct_io,
          size_ec ? 0 : static_cast<std::uint64_t>(size));
      file_.emplace(ctx_->driver(), path_, plan_.direct_io);
      scope_.emplace(*h_, ctx_->host(), *file_, sched_, plan_,
                     blake3pp::detail::pipeline_options{
                         .file_index = file_index_,
                         .stop_token = stop_.get_token()},
                     tracer_);
      cells_.emplace(scope_->count());
      live_ = scope_->begin(watch_);
    } catch (...) {
      eptr_ = std::current_exception();
      live_ = false;
    }
  }

 public:
  [[nodiscard]] bool finished() const noexcept override {
    return !live_ || scope_->status().done;
  }

  [[nodiscard]] unsigned outstanding() const noexcept override {
    return live_ ? scope_->status().outstanding : 0;
  }

  void request_stop() noexcept override { stop_.request_stop(); }

  void report() noexcept override {
    // The receiver's stop is done with before anything is reported: the
    // callback's destructor waits out one already running.
    rcvr_watch_.reset();
    if (!live_) {
      // Never ran, or gave up mid-round.
      watch_.reset();
      if (eptr_) {
        blake3pp::ex::set_error(std::move(rcvr_), std::move(eptr_));
        return;
      }
      // begin() said there was nothing to drive: an empty file, or a
      // token that was already stopped.
      if (scope_ && scope_->stopped()) {
        blake3pp::ex::set_stopped(std::move(rcvr_));
        return;
      }
      complete_value();
      return;
    }
    // end() drains the reducer into the hasher, so the contract checks on
    // that path (contract.hpp) throw from here rather than from a stage;
    // the blocking entry points let that propagate out of run(), and this
    // one has the sender's own error channel for it.
    typename scope_type::outcome done = scope_type::outcome::value;
    try {
      done = scope_->end(watch_);
    } catch (...) {
      blake3pp::ex::set_error(std::move(rcvr_), std::current_exception());
      return;
    }
    switch (done) {
      case scope_type::outcome::exception:
        blake3pp::ex::set_error(std::move(rcvr_), scope_->exception());
        return;
      case scope_type::outcome::error:
        blake3pp::ex::set_error(std::move(rcvr_), scope_->error());
        return;
      case scope_type::outcome::stopped:
        blake3pp::ex::set_stopped(std::move(rcvr_));
        return;
      case scope_type::outcome::value:
        complete_value();
        return;
    }
  }

 private:
  using receiver_token =
      blake3pp::ex::stop_token_of_t<blake3pp::ex::env_of_t<Receiver>>;
  using receiver_watch =
      blake3pp::ex::stop_callback_for_t<receiver_token, forward_stop>;

  void watch_receiver_stop() noexcept {
    auto token = blake3pp::ex::get_stop_token(blake3pp::ex::get_env(rcvr_));
    if constexpr (blake3pp::ex::unstoppable_token<receiver_token>) {
      (void)token;
    } else {
      rcvr_watch_.emplace(std::move(token), forward_stop{&stop_});
    }
  }

  void complete_value() noexcept {
    if constexpr (std::is_void_v<Value>) {
      blake3pp::ex::set_value(std::move(rcvr_));
    } else {
      // The hasher is the operation's until it completes, so finalizing
      // it here is safe and is what async_hash_file is for.
      blake3pp::ex::set_value(std::move(rcvr_), h_->finalize());
    }
  }

  basic_io_context<Budget>* ctx_;
  bool opened_ = false;
  // Declared before h_, which points into it when this operation owns
  // its hasher.
  std::optional<hasher> own_h_;
  hasher* h_;
  std::filesystem::path path_;
  Sched sched_;
  file_io_options opts_;
  Tracer tracer_;
  std::uint32_t file_index_;
  Receiver rcvr_;
  blake3pp::ex::inplace_stop_source stop_;
  blake3pp::detail::pipeline_plan plan_{};
  std::optional<io_driver::file> file_;
  std::optional<scope_type> scope_;
  std::optional<cells_type> cells_;
  typename scope_type::stop_watch watch_;
  std::optional<receiver_watch> rcvr_watch_;
  std::exception_ptr eptr_;
  bool live_ = false;
};

// set_value_t() for the update form and set_value_t(digest) for the hash
// form. A conditional_t cannot do it: the branch not taken would still
// have to name set_value_t(void), which is not a type.
template <class Value>
struct value_signature {
  using type = blake3pp::ex::set_value_t(Value);
};
template <>
struct value_signature<void> {
  using type = blake3pp::ex::set_value_t();
};

// What a sender of this shape says about itself: every completion
// arrives on the context's thread, which is what the scheduler below
// names. An adaptor that wants to stay on that thread can read it
// instead of guessing.
struct sender_env {
  using scheduler_type =
      blake3pp::detail::driver_scheduler<blake3pp::detail::io_driver>;
  scheduler_type sched;

  template <class Tag>
  [[nodiscard]] scheduler_type query(
      blake3pp::ex::get_completion_scheduler_t<Tag>) const noexcept {
    return sched;
  }
};

// The sender for both entry points: the value is a digest or nothing,
// and everything else about them is the same.
template <stack_budget Budget, class Sched, class Value, class Options,
          tracer Tracer = null_tracer>
class update_file_sender {
 public:
  using sender_concept = blake3pp::detail::ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(
      typename value_signature<Value>::type,
      blake3pp::ex::set_error_t(std::error_code),
      blake3pp::ex::set_error_t(std::exception_ptr),
      blake3pp::ex::set_stopped_t());

  // file_index is what this file's window records name as their file:
  // the batch operation's slot index, zero for a file on its own.
  update_file_sender(basic_io_context<Budget>& ctx, hasher* h,
                     std::filesystem::path path, Sched sched, Options opts,
                     Tracer tracer = Tracer{}, std::uint32_t file_index = 0)
      : ctx_(&ctx),
        h_(h),
        path_(std::move(path)),
        sched_(std::move(sched)),
        opts_(opts),
        tracer_(tracer),
        file_index_(file_index) {}

  [[nodiscard]] sender_env get_env() const noexcept {
    return {ctx_->scheduler()};
  }

  // The scope and its cell store, named here so connect() can spell the
  // operation state's type. The tracer is part of that type, which is
  // why it is an argument of the sender rather than a field of the
  // options.
  using scope_type =
      blake3pp::detail::window_scope<Tracer, Budget, io_driver, Sched>;
  using cells_type = blake3pp::detail::pipeline_cells_for<scope_type>;

  template <class Receiver>
  [[nodiscard]] update_file_op<Budget, Sched, Receiver, Value, cells_type,
                               Tracer>
  connect(Receiver r) const {
    if constexpr (std::is_void_v<Value>) {
      return {*ctx_, *h_, path_, sched_, opts_, tracer_, file_index_,
              std::move(r)};
    } else {
      return {*ctx_, path_, sched_, opts_, tracer_, file_index_,
              std::move(r)};
    }
  }

 private:
  basic_io_context<Budget>* ctx_;
  hasher* h_;
  std::filesystem::path path_;
  Sched sched_;
  Options opts_;
  Tracer tracer_;
  std::uint32_t file_index_;
};

}  // namespace detail

/// Streams a file into a hasher over a context, as a sender.
///
/// Completes with set_value(), set_error(std::error_code),
/// set_error(std::exception_ptr) or set_stopped(), always on the thread
/// running the context.
///
/// @param ctx    The context whose windows and driver this borrows. It
///               must outlive the operation.
/// @param h      The hasher to stream into. It must outlive the
///               operation, and nothing else may touch it while this
///               runs; it keeps its mode, so keyed and derive_key
///               hashers work as they do everywhere else.
/// @param path   The file to read; copied.
/// @param sched  Where the subtree compressions run.
/// @param opts   Per-call knobs. The window geometry is the context's;
///               what is left here is direct_io, which can only turn the
///               context's default off. `opts.trace` is not consulted:
///               tracing is the next argument.
/// @param tracer How to record (<blake3pp/trace.hpp>): the default
///               null_tracer records nothing; buffer_tracer{&buf} fills
///               the buffer with this file's window records. A type
///               rather than an option, because it becomes part of the
///               operation state's type.
template <stack_budget Budget, class Sched, tracer Tracer = null_tracer>
  requires ex::scheduler<std::remove_cvref_t<Sched>>
[[nodiscard]] auto async_update_file(basic_io_context<Budget>& ctx, hasher& h,
                                     std::filesystem::path path, Sched&& sched,
                                     const file_io_options& opts = {},
                                     Tracer tracer = {}) {
  return detail::update_file_sender<Budget, std::remove_cvref_t<Sched>, void,
                                    file_io_options, Tracer>(
      ctx, &h, std::move(path), std::forward<Sched>(sched), opts, tracer);
}

/// Hashes a whole file over a context, as a sender.
///
/// Completes with set_value(digest), set_error(std::error_code),
/// set_error(std::exception_ptr) or set_stopped(), always on the thread
/// running the context. The hasher is the operation's own.
///
/// @param ctx    The context whose windows and driver this borrows.
/// @param path   The file to read; copied.
/// @param sched  Where the subtree compressions run.
/// @param opts   Per-call knobs: the arch, the mode, and direct_io. Copied
///               into the operation, mode included, so nothing it names
///               has to outlive the call. `opts.trace` is not consulted.
/// @param tracer How to record, as for async_update_file().
template <stack_budget Budget, class Sched, tracer Tracer = null_tracer>
  requires ex::scheduler<std::remove_cvref_t<Sched>>
[[nodiscard]] auto async_hash_file(basic_io_context<Budget>& ctx,
                                   std::filesystem::path path, Sched&& sched,
                                   const hash_file_options& opts = {},
                                   Tracer tracer = {}) {
  return detail::update_file_sender<Budget, std::remove_cvref_t<Sched>, digest,
                                    hash_file_options, Tracer>(
      ctx, nullptr, std::move(path), std::forward<Sched>(sched), opts, tracer);
}

/// How one file of a many-files operation ended. The outcome is reported
/// to the caller's callback; the operation itself completes once, after
/// every file has an outcome.
struct file_outcome {
  /// The error_code channel: an open or a read that failed.
  std::error_code error{};
  /// The exception channel: everything else that went wrong.
  std::exception_ptr exception{};
  /// The file was stopped, or never started because a stop came first.
  bool stopped = false;
  /// Whether the hasher holds the whole file.
  [[nodiscard]] bool ok() const noexcept {
    return !error && exception == nullptr && !stopped;
  }
};

/// Knobs for async_update_files().
struct many_files_options {
  /// Files in flight at once. Zero means as many as the context has
  /// windows, so that every file in flight can hold a window; the
  /// context's window admission shares them out from there. A file
  /// smaller than one window is one read, so this is the count that
  /// keeps a device and a pool busy over many small files.
  unsigned in_flight = 0;
  /// Per-file knobs, as for async_update_file().
  file_io_options io{};
};

namespace detail {

// Many files over one context, a bounded number in flight: a node that
// owns up to `in_flight` child operations and refills a slot from its
// own round once the child in it has reported. The child reports from
// inside the loop's report phase and the slot is recycled in the next
// round, so no operation state is destroyed inside its own completion.
//
// Each file's outcome goes to the callback with the hasher that
// streamed it; the operation completes once, with set_value() after
// the last outcome, or set_stopped() when a stop ended it. Per-file
// failures are outcomes, not completions: one unreadable file does not
// end the run.
template <stack_budget Budget, class Sched, class MakeHasher, class OnDone,
          class Receiver, tracer Tracer>
class update_files_op final : public pipeline_node {
 public:
  using operation_state_concept =
      blake3pp::detail::ex_compat::operation_state_tag;
  using child_sender =
      update_file_sender<Budget, Sched, void, file_io_options, Tracer>;

  update_files_op(basic_io_context<Budget>& ctx,
                  std::span<const std::filesystem::path> paths, Sched sched,
                  const many_files_options& opts, MakeHasher make, OnDone done,
                  Tracer tracer, Receiver r)
      : ctx_(&ctx),
        paths_(paths),
        sched_(std::move(sched)),
        opts_(opts),
        make_(std::move(make)),
        done_(std::move(done)),
        tracer_(tracer),
        rcvr_(std::move(r)) {}

  update_files_op(const update_files_op&) = delete;
  update_files_op& operator=(const update_files_op&) = delete;

  void start() noexcept {
    // One slot per window rather than one per full read queue: a slot
    // reserving a queue of windows under-counts the files exactly where
    // files are small and need one read each. Measured on 32 cores over
    // 2000 x 1 MiB files: 3.7 GiB/s at two slots, 11.6 at eight, 15.0 at
    // sixteen, and within 5% of that from there to sixty-four.
    const unsigned width =
        opts_.in_flight != 0 ? opts_.in_flight : std::max(1u, ctx_->windows());
    watch_receiver_stop();
    try {
      // Slots hold operation states, which do not move: one allocation,
      // sized once, never grown.
      width_ = std::min<std::size_t>(width, paths_.size());
      slots_ = std::make_unique<slot[]>(width_);
    } catch (...) {
      eptr_ = std::current_exception();
    }
    ctx_->link(this);
  }

  void round() noexcept override {
    if (eptr_ != nullptr) {
      return;
    }
    for (slot& s : std::span{slots_.get(), width_}) {
      if (s.reported) {
        s.op.reset();
        s.h.reset();
        s.reported = false;
      }
      if (!s.op && !stop_.stop_requested() && next_ < paths_.size()) {
        start_slot(s, next_++);
      }
    }
    if (stop_.stop_requested()) {
      // Files that never started are stopped outcomes, reported in order.
      while (next_ < paths_.size()) {
        done_(next_, spare_, file_outcome{.stopped = true});
        ++next_;
        ++reported_;
      }
    }
  }

  [[nodiscard]] bool finished() const noexcept override {
    return eptr_ != nullptr || reported_ == paths_.size();
  }

  [[nodiscard]] unsigned outstanding() const noexcept override { return 0; }

  // From the context's own request_stop, on the running thread. The
  // receiver's token arrives through the same source from any thread.
  void request_stop() noexcept override { stop_.request_stop(); }

  void report() noexcept override {
    // The receiver's stop is done with before anything is reported: the
    // callback's destructor waits out one already running.
    rcvr_watch_.reset();
    if (eptr_ != nullptr) {
      blake3pp::ex::set_error(std::move(rcvr_), std::move(eptr_));
    } else if (stop_.stop_requested()) {
      blake3pp::ex::set_stopped(std::move(rcvr_));
    } else {
      blake3pp::ex::set_value(std::move(rcvr_));
    }
  }

 private:
  struct slot;

  // The environment a child runs in: the parent's stop token, so a stop
  // the parent took -- the context's, or the receiver's -- reaches every
  // file already in flight through the child's own watch, and not only the
  // files not yet started.
  struct child_env {
    blake3pp::ex::inplace_stop_token token;
    [[nodiscard]] blake3pp::ex::inplace_stop_token query(
        blake3pp::ex::get_stop_token_t) const noexcept {
      return token;
    }
  };

  // What a child completes into: the slot's outcome, through the parent.
  struct child_receiver {
    using receiver_concept = blake3pp::detail::ex_compat::receiver_tag;
    child_receiver(update_files_op* op, slot* s) noexcept : op_(op), s_(s) {}
    [[nodiscard]] child_env get_env() const noexcept {
      return {op_->stop_.get_token()};
    }
    void set_value() const noexcept { op_->child_done(*s_, {}); }
    void set_error(std::error_code ec) const noexcept {
      op_->child_done(*s_, file_outcome{.error = ec});
    }
    void set_error(std::exception_ptr e) const noexcept {
      op_->child_done(*s_, file_outcome{.exception = std::move(e)});
    }
    void set_stopped() const noexcept {
      op_->child_done(*s_, file_outcome{.stopped = true});
    }

   private:
    update_files_op* op_;
    slot* s_;
  };
  using child_op = blake3pp::ex::connect_result_t<child_sender, child_receiver>;

  // A stop from the receiver -- a when_all sibling failing, a stop_when --
  // requests the same source the context's request_stop() does, and nudges
  // the driver: the round that reports the files that never started is the
  // one this has to bring about.
  struct forward_stop_and_wake {
    update_files_op* op;
    void operator()() const noexcept {
      op->stop_.request_stop();
      op->ctx_->driver().wake();
    }
  };
  using receiver_token =
      blake3pp::ex::stop_token_of_t<blake3pp::ex::env_of_t<Receiver>>;
  using receiver_watch =
      blake3pp::ex::stop_callback_for_t<receiver_token, forward_stop_and_wake>;

  void watch_receiver_stop() noexcept {
    auto token = blake3pp::ex::get_stop_token(blake3pp::ex::get_env(rcvr_));
    if constexpr (blake3pp::ex::unstoppable_token<receiver_token>) {
      (void)token;
    } else {
      rcvr_watch_.emplace(std::move(token), forward_stop_and_wake{this});
    }
  }

  struct slot {
    std::size_t index = 0;
    std::optional<hasher> h;
    std::optional<child_op> op;
    bool reported = false;
  };

  // Builds the child in place: an operation state is immovable, so the
  // optional takes a conversion that constructs it where it will live.
  struct child_connect {
    update_files_op* op;
    slot* s;
    operator child_op() const {
      return blake3pp::ex::connect(
          child_sender(*op->ctx_, &*s->h, op->paths_[s->index], op->sched_,
                       op->opts_.io, op->tracer_,
                       static_cast<std::uint32_t>(s->index)),
          child_receiver{op, s});
    }
  };

  void start_slot(slot& s, std::size_t index) noexcept {
    s.index = index;
    try {
      s.h.emplace(make_(index));
      s.op.emplace(child_connect{this, &s});
    } catch (...) {
      // The file never started: its outcome is the exception, and the
      // slot is free again.
      s.h.reset();
      s.op.reset();
      done_(index, spare_, file_outcome{.exception = std::current_exception()});
      ++reported_;
      return;
    }
    blake3pp::ex::start(*s.op);
  }

  void child_done(slot& s, file_outcome o) noexcept {
    done_(s.index, *s.h, o);
    s.reported = true;
    ++reported_;
    // The slot is refilled in this node's next round, and the loop may
    // be about to park with nothing owed: the nudge is what brings that
    // round.
    ctx_->driver().wake();
  }

  basic_io_context<Budget>* ctx_;
  std::span<const std::filesystem::path> paths_;
  Sched sched_;
  many_files_options opts_;
  MakeHasher make_;
  OnDone done_;
  Tracer tracer_;
  Receiver rcvr_;
  std::unique_ptr<slot[]> slots_;
  std::size_t width_ = 0;
  hasher spare_{};  // what a file that never started is reported with
  std::size_t next_ = 0;
  std::size_t reported_ = 0;
  // The one stop of this run: the context's request_stop() and the
  // receiver's token both go through it, and every child reads it from its
  // environment.
  blake3pp::ex::inplace_stop_source stop_;
  std::optional<receiver_watch> rcvr_watch_;
  std::exception_ptr eptr_;
};

template <stack_budget Budget, class Sched, class MakeHasher, class OnDone,
          tracer Tracer = null_tracer>
class update_files_sender {
 public:
  using sender_concept = blake3pp::detail::ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(blake3pp::ex::set_value_t(),
                                    blake3pp::ex::set_error_t(std::exception_ptr),
                                    blake3pp::ex::set_stopped_t());

  update_files_sender(basic_io_context<Budget>& ctx,
                      std::span<const std::filesystem::path> paths,
                      Sched sched, const many_files_options& opts,
                      MakeHasher make, OnDone done, Tracer tracer = Tracer{})
      : ctx_(&ctx),
        paths_(paths),
        sched_(std::move(sched)),
        opts_(opts),
        make_(std::move(make)),
        done_(std::move(done)),
        tracer_(tracer) {}

  [[nodiscard]] sender_env get_env() const noexcept {
    return {ctx_->scheduler()};
  }

  template <class Receiver>
  [[nodiscard]] update_files_op<Budget, Sched, MakeHasher, OnDone, Receiver,
                                Tracer>
  connect(Receiver r) const {
    return {*ctx_, paths_, sched_, opts_, make_, done_, tracer_,
            std::move(r)};
  }

 private:
  basic_io_context<Budget>* ctx_;
  std::span<const std::filesystem::path> paths_;
  Sched sched_;
  many_files_options opts_;
  MakeHasher make_;
  OnDone done_;
  Tracer tracer_;
};

// Where sync_wait() parks a completion. `done` is atomic and the
// waiting thread is woken through the driver, because the last stage of
// a composed sender need not run on the context's thread: a
// continues_on() to a pool scheduler finishes this chain somewhere else,
// and a plain flag read by a thread parked in poll() would never be
// seen.
template <class Tuple>
struct sync_state {
  std::optional<Tuple> value;
  std::exception_ptr error;
  bool stopped = false;
  std::atomic<bool> done{false};
};

#if defined(BLAKE3PP_TEST_IO_SEAMS)
// Runs between sync_wait()'s receiver setting its flag and waking the
// context, which is where a waiter that did not wait for the wake would
// return and destroy what the wake still reaches. Test builds only; the
// library never instantiates sync_wait().
inline namespace test_seams {
inline void (*before_sync_wake)() noexcept = nullptr;
}
#endif

using sync_loop = blake3pp::detail::driver_loop<blake3pp::detail::io_driver>;

template <class Tuple>
struct sync_receiver {
  using receiver_concept = blake3pp::detail::ex_compat::receiver_tag;
  sync_receiver(sync_state<Tuple>* s, sync_loop* loop) noexcept
      : st_(s), loop_(loop) {}
  template <class... Ts>
  void set_value(Ts&&... ts) const noexcept {
    try {
      st_->value.emplace(std::forward<Ts>(ts)...);
    } catch (...) {
      st_->error = std::current_exception();
    }
    finish();
  }
  void set_error(std::error_code ec) const noexcept {
    st_->error = std::make_exception_ptr(std::system_error(ec));
    finish();
  }
  void set_error(std::exception_ptr e) const noexcept {
    st_->error = std::move(e);
    finish();
  }
  void set_stopped() const noexcept {
    st_->stopped = true;
    finish();
  }

 private:
  // The flag last, after everything the waiting thread will read, and the
  // wake after the flag. The waiter may return as soon as it sees the
  // flag, destroying this receiver with its operation and then possibly
  // the context, so the loop is held as a publisher across both: its
  // run_until does not return until the wake is over. The loop pointer is
  // copied out first, because the receiver may be gone by the time the
  // pin is released.
  void finish() const noexcept {
    sync_loop* const loop = loop_;
    const sync_loop::publishing pin{loop};
    st_->done.store(true, std::memory_order_release);
#if defined(BLAKE3PP_TEST_IO_SEAMS)
    if (before_sync_wake != nullptr) {
      before_sync_wake();
    }
#endif
    loop->driver().wake();
  }

  sync_state<Tuple>* st_;
  sync_loop* loop_;
};

}  // namespace detail

/// Streams many files into hashers over a context, a bounded number at a
/// time, as a sender. Each file's hasher comes from `make(index)`; when
/// the file has been streamed, or has failed, `done(index, h, outcome)`
/// is called on the context's thread with that hasher and the outcome,
/// in whatever order the files finish. The sender completes once, with
/// set_value() after the last outcome, set_error(exception_ptr) if its
/// own bookkeeping failed, or set_stopped() when a stop ended the run,
/// in which case files that never started are reported as stopped.
///
/// @param ctx    The context whose windows and driver this borrows.
/// @param paths  The files; the span must outlive the operation.
/// @param sched  Where the subtree compressions run.
/// @param opts   How many files run at once, and the per-file knobs.
/// @param make   `hasher(std::size_t index)`: the hasher for a file,
///               with its mode (plain, keyed, derive_key) and variant.
/// @param done   `void(std::size_t index, hasher& h, const
///               file_outcome&) noexcept`: the file's end.
/// @param tracer How to record, as for async_update_file(): one buffer
///               for every file in flight, whose window records carry
///               the window index within its file. `opts.io.trace` is
///               not consulted.
template <stack_budget Budget, class Sched, class MakeHasher, class OnDone,
          tracer Tracer = null_tracer>
  requires ex::scheduler<std::remove_cvref_t<Sched>> &&
           std::is_invocable_r_v<hasher, MakeHasher&, std::size_t> &&
           std::is_nothrow_invocable_v<OnDone&, std::size_t, hasher&,
                                       const file_outcome&>
[[nodiscard]] auto async_update_files(basic_io_context<Budget>& ctx,
                                      std::span<const std::filesystem::path> paths,
                                      Sched&& sched,
                                      const many_files_options& opts,
                                      MakeHasher make, OnDone done,
                                      Tracer tracer = {}) {
  return detail::update_files_sender<Budget, std::remove_cvref_t<Sched>,
                                     MakeHasher, OnDone, Tracer>(
      ctx, paths, std::forward<Sched>(sched), opts, std::move(make),
      std::move(done), tracer);
}

/// Starts `sndr` and drives the context on this thread until it
/// completes. Returns the values as a tuple, an empty optional for
/// set_stopped(), and rethrows an error (an error_code arrives as
/// std::system_error). The sender must have exactly one value
/// completion, as for ex::sync_wait; any sender over this context's
/// operations qualifies, composed or not, and it may complete on any
/// thread. Other operations started on the context keep running meanwhile
/// and may still be in flight when this returns. The completion is over
/// when this returns, wake included: with nothing else in flight, the
/// context may be destroyed straight away.
template <stack_budget Budget, class Sender>
auto sync_wait(basic_io_context<Budget>& ctx, Sender&& sndr) {
  // Asking the sender what it completes with needs a receiver environment
  // to ask against, and the answer is what names the tuple this returns.
  // Any sync_receiver's environment will do -- the tuple is not part of
  // it -- so the query uses a throwaway instantiation rather than the one
  // built below, which cannot be named until the tuple is known.
  using env_t = decltype(blake3pp::ex::get_env(
      std::declval<const detail::sync_receiver<std::tuple<>>&>()));
  using tuple_t = blake3pp::ex::value_types_of_t<std::remove_cvref_t<Sender>,
                                                 env_t, std::tuple,
                                                 std::type_identity_t>;
  detail::sync_state<tuple_t> st;
  auto op = blake3pp::ex::connect(
      std::forward<Sender>(sndr),
      detail::sync_receiver<tuple_t>{&st, &ctx.host().loop()});
  bool started = false;
  try {
    ctx.run_until(st.done, [&]() noexcept {
      started = true;
      blake3pp::ex::start(op);
    });
  } catch (...) {
    // The context was not this thread's to drive -- another thread is in
    // run(), or this is a completion on the context's own thread -- and
    // nothing was started: the misuse is reported, nothing wound down.
    if (!started) {
      throw;
    }
    // `op` is started and unfinished, and the context still has it
    // linked: destroying it on the way out of this frame would leave the
    // loop walking a dead node. So the run is stopped and wound down
    // first, the same way a failing round winds its own scope down.
    ctx.request_stop();
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      if (st.done.load(std::memory_order_acquire)) {
        break;
      }
      try {
        ctx.run_until_idle();
      } catch (...) {
        // Whatever the driver is doing, it is not getting better by
        // being asked differently; the loop below decides.
      }
    }
    if (!st.done.load(std::memory_order_acquire)) {
      std::fprintf(stderr,
                   "blake3pp: an io_context could not be wound down while a "
                   "sync_wait() operation was still in flight; it cannot be "
                   "destroyed without leaving the context a dead node\n");
      std::abort();
    }
    // A drive() that threw before reaching the loop never waited for the
    // receiver's wake; nothing may be destroyed until it is over.
    ctx.host().loop().settle_publishers();
    throw;
  }
  if (st.error != nullptr) {
    std::rethrow_exception(st.error);
  }
  if (st.stopped) {
    return std::optional<tuple_t>{};
  }
  return std::move(st.value);
}

}  // namespace blake3pp::async
