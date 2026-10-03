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
/// thread runs the context, and every operation completes on it.
///
/// ```cpp
/// blake3pp::async::io_context ctx;
/// exec::static_thread_pool pool{std::thread::hardware_concurrency()};
///
/// auto work = stdexec::when_all(
///     blake3pp::async::async_hash_file(ctx, "a.bin", pool.get_scheduler()),
///     blake3pp::async::async_hash_file(ctx, "b.bin", pool.get_scheduler()));
/// auto [digests] = stdexec::sync_wait(std::move(work)).value();
/// ```
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

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>
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
  /// Bytes per window; rounded down to a power-of-2 multiple of
  /// chunk_size, minimum 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  /// Windows one operation may hold at once; clamped to [2, 32].
  unsigned queue_depth = 4;
  /// Windows in the shared arena. Zero means queue_depth, which is one
  /// operation's worth; a context that runs several files at once wants
  /// more, at window_bytes each.
  unsigned windows = 0;
  /// Bypass the page cache where the platform supports it, per file.
  /// A file whose window grid is unaligned is opened buffered whatever
  /// this says, because every window after an unaligned one starts
  /// unaligned too.
  bool direct_io = true;
  /// Issue reads on the kernel's I/O worker threads; see
  /// file_io_options.
  bool offload_submit = true;
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
/// only call request_stop(). Operations complete on the running thread,
/// inside the run call, which is also the only place a receiver of one
/// of these senders is ever invoked.
///
/// @tparam Budget The stack a window's part table may take. It is the
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
        drv_({/*async=*/true, opts.offload_submit}, pool_windows(opts, plan_)),
        host_(drv_, pool_windows(opts, plan_), plan_.window_bytes) {}

  basic_io_context(const basic_io_context&) = delete;
  basic_io_context& operator=(const basic_io_context&) = delete;

  ~basic_io_context() {
    // P2300's rule: an operation state may not be destroyed between
    // start() and its completion, and the context is what would have
    // completed it.
    assert(head_ == nullptr &&
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

  // Joins the loop's round-robin. The node is the caller's; it leaves
  // the list when it reports.
  void link(detail::pipeline_node* n) noexcept {
    n->next = head_;
    head_ = n;
    ++live_;
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
    return opts.windows != 0 ? opts.windows : plan.windows;
  }

  void drive(bool until_idle) {
    const bool was_running = running_.exchange(true, std::memory_order_acq_rel);
    assert(!was_running &&
           "only one thread may run an io_context; the others may call "
           "request_stop() and nothing else");
    (void)was_running;
    blake3pp::detail::loop_status status{};
    host_.loop().run_until(
        status,
        [&] {
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
          status.done = live_ == 0 && (until_idle || stop_.load(
                                                         std::memory_order_acquire));
        },
        [&] {
          // Nothing owed, nothing to run, and nothing the driver can
          // deliver. For a context that is not a misconfiguration: it is
          // a thread waiting for work or for a stop, so it sleeps.
          // run_until only calls this when it is about to block, and a
          // wake() from link() or request_stop() is what ends it.
        });
    running_.store(false, std::memory_order_release);
    stop_.store(false, std::memory_order_release);
  }

  io_context_options opts_;
  blake3pp::detail::pipeline_plan plan_;
  blake3pp::detail::io_driver drv_;
  blake3pp::detail::pipeline_host<Budget, blake3pp::detail::io_driver> host_;
  detail::pipeline_node* head_ = nullptr;
  unsigned live_ = 0;
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
// Nothing completes inside start(). A failure there -- an open that
// could not be made, a plan that cannot run -- is latched and reported
// from the first round, which is the same rule every stage inside the
// pipeline already obeys.
template <stack_budget Budget, class Sched, class Receiver, class Value>
class update_file_op final : public pipeline_node {
 public:
  using operation_state_concept = blake3pp::detail::ex_compat::operation_state_tag;
  using scope_type =
      blake3pp::detail::window_scope<false, Budget, io_driver, Sched>;
  using cells_type = blake3pp::detail::pipeline_cells<scope_type>;

  // Over a hasher the caller owns: it must outlive the operation, and
  // nothing else may touch it while this runs.
  update_file_op(basic_io_context<Budget>& ctx, hasher& h,
                 std::filesystem::path path, Sched sched,
                 const file_io_options& opts, Receiver r)
      : ctx_(&ctx),
        h_(&h),
        path_(std::move(path)),
        sched_(std::move(sched)),
        opts_(opts),
        rcvr_(std::move(r)) {}

  // Over a hasher of its own, built from the call's options and
  // finalized into the value this completes with.
  update_file_op(basic_io_context<Budget>& ctx, std::filesystem::path path,
                 Sched sched, const hash_file_options& opts, Receiver r)
      : ctx_(&ctx),
        own_h_(blake3pp::detail::make_hasher(opts, blake3pp::detail::resolve(opts.a))),
        h_(&*own_h_),
        path_(std::move(path)),
        sched_(std::move(sched)),
        opts_(opts),
        rcvr_(std::move(r)) {}

  update_file_op(const update_file_op&) = delete;
  update_file_op& operator=(const update_file_op&) = delete;

  void start() noexcept {
    // The receiver's stop, bridged onto the source the scope watches.
    // Installed before anything is opened, so a token already stopped
    // takes the same path as one stopped mid-file.
    watch_receiver_stop();
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
                         .window_bytes = cfg.window_bytes,
                         .queue_depth = cfg.queue_depth,
                         .trace = nullptr,
                         .stop_token = stop_.get_token()});
      cells_.emplace(scope_->count());
      live_ = scope_->begin(watch_);
    } catch (...) {
      eptr_ = std::current_exception();
      live_ = false;
    }
    ctx_->link(this);
  }

  void round() noexcept override {
    if (!live_) {
      return;
    }
    try {
      scope_->admit_round(*cells_);
    } catch (...) {
      // The round threw, so the cells cannot simply be destroyed: a
      // window on the pool may be about to publish into one. Wait those
      // out first, then report from the next round.
      scope_->abandon();
      eptr_ = std::current_exception();
      live_ = false;
    }
  }

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
    switch (scope_->end(watch_)) {
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
  // Declared before h_, which points into it when this operation owns
  // its hasher.
  std::optional<hasher> own_h_;
  hasher* h_;
  std::filesystem::path path_;
  Sched sched_;
  file_io_options opts_;
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
template <stack_budget Budget, class Sched, class Value, class Options>
class update_file_sender {
 public:
  using sender_concept = blake3pp::detail::ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(
      typename value_signature<Value>::type,
      blake3pp::ex::set_error_t(std::error_code),
      blake3pp::ex::set_error_t(std::exception_ptr),
      blake3pp::ex::set_stopped_t());

  update_file_sender(basic_io_context<Budget>& ctx, hasher* h,
                     std::filesystem::path path, Sched sched, Options opts)
      : ctx_(&ctx),
        h_(h),
        path_(std::move(path)),
        sched_(std::move(sched)),
        opts_(opts) {}

  [[nodiscard]] sender_env get_env() const noexcept {
    return {ctx_->scheduler()};
  }

  template <class Receiver>
  [[nodiscard]] update_file_op<Budget, Sched, Receiver, Value> connect(
      Receiver r) const {
    if constexpr (std::is_void_v<Value>) {
      return {*ctx_, *h_, path_, sched_, opts_, std::move(r)};
    } else {
      return {*ctx_, path_, sched_, opts_, std::move(r)};
    }
  }

 private:
  basic_io_context<Budget>* ctx_;
  hasher* h_;
  std::filesystem::path path_;
  Sched sched_;
  Options opts_;
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
/// @param path   The file to read. It must outlive the operation.
/// @param sched  Where the subtree compressions run.
/// @param opts   Per-call knobs. The window geometry is the context's;
///               what is left here is direct_io, which can only turn the
///               context's default off.
template <stack_budget Budget, class Sched>
  requires ex::scheduler<std::remove_cvref_t<Sched>>
[[nodiscard]] auto async_update_file(basic_io_context<Budget>& ctx, hasher& h,
                                     std::filesystem::path path, Sched&& sched,
                                     const file_io_options& opts = {}) {
  return detail::update_file_sender<Budget, std::remove_cvref_t<Sched>, void,
                                    file_io_options>(
      ctx, &h, std::move(path), std::forward<Sched>(sched), opts);
}

/// Hashes a whole file over a context, as a sender.
///
/// Completes with set_value(digest), set_error(std::error_code),
/// set_error(std::exception_ptr) or set_stopped(), always on the thread
/// running the context. The hasher is the operation's own, so nothing
/// but the path has to outlive the call.
///
/// @param ctx    The context whose windows and driver this borrows.
/// @param path   The file to read. It must outlive the operation.
/// @param sched  Where the subtree compressions run.
/// @param opts   Per-call knobs: the arch, a key, and direct_io.
template <stack_budget Budget, class Sched>
  requires ex::scheduler<std::remove_cvref_t<Sched>>
[[nodiscard]] auto async_hash_file(basic_io_context<Budget>& ctx,
                                   std::filesystem::path path, Sched&& sched,
                                   const hash_file_options& opts = {}) {
  return detail::update_file_sender<Budget, std::remove_cvref_t<Sched>, digest,
                                    hash_file_options>(
      ctx, nullptr, std::move(path), std::forward<Sched>(sched), opts);
}

}  // namespace blake3pp::async
