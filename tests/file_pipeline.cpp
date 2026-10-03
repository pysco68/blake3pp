// The sender/receiver ground the file pipeline stands on: can this
// build's provider host a custom sender, a custom scheduler and their
// operation states, and do they compose with then, let_value and
// continues_on?
//
// These are conformance cases, not pipeline cases. They exist because
// the answer differs per provider and the diagnostics when it goes wrong
// name provider internals rather than the sender at fault; see
// <blake3pp/detail/ex_compat.hpp>, whose rules each come from one of the
// failures below.

#include <cstdio>
#include <exception>
#include <memory>
#include <atomic>
#include <bit>
#include <concepts>
#include <string_view>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <vector>

#include "test_schedulers.hpp"

#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/io_driver.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_io.hpp>
#include <doctest/doctest.h>

namespace {

namespace ex = blake3pp::ex;
namespace compat = blake3pp::detail::ex_compat;

[[nodiscard]] constexpr std::string_view provider_name() noexcept {
#if defined(BLAKE3PP_EXECUTION_STD)
  return "std::execution";
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
  return "beman.execution";
#else
  return "stdexec";
#endif
}

// A custom sender: one value, one error type.
template <class Receiver>
struct int_op {
  using operation_state_concept = compat::operation_state_tag;
  Receiver rcvr;
  int value;
  // RULE 2: the CPO, not the member.
  void start() & noexcept { ex::set_value(std::move(rcvr), value); }
};

struct int_sender {
  using sender_concept = compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t(int),
                                    ex::set_error_t(std::error_code));
  int value = 0;

  template <class Receiver>
  int_op<Receiver> connect(Receiver r) const {
    return {std::move(r), value};
  }
};

// A custom scheduler: a run loop the caller drives, which is the shape
// the pipeline's driver scheduler will have.
struct run_node {
  run_node* next = nullptr;
  void (*run)(run_node*) noexcept = nullptr;
};

struct run_queue {
  run_node* head = nullptr;
  void push(run_node* n) noexcept {
    n->next = head;
    head = n;
  }
  bool drain_one() noexcept {
    if (head == nullptr) {
      return false;
    }
    run_node* const n = head;
    head = n->next;
    n->run(n);
    return true;
  }
};

struct test_scheduler;

template <class Receiver>
struct sched_op : run_node {
  using operation_state_concept = compat::operation_state_tag;
  Receiver rcvr;
  run_queue* q;
  sched_op(Receiver r, run_queue* queue) : rcvr(std::move(r)), q(queue) {
    run = [](run_node* self) noexcept {
      auto* const me = static_cast<sched_op*>(self);
      ex::set_value(std::move(me->rcvr));
    };
  }
  void start() & noexcept { q->push(this); }
};

struct sched_env {
  run_queue* q;
  [[nodiscard]] test_scheduler query(
      ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept;
};

struct sched_sender {
  using sender_concept = compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t());
  run_queue* q;

  template <class Receiver>
  sched_op<Receiver> connect(Receiver r) const {
    return {std::move(r), q};
  }
  [[nodiscard]] sched_env get_env() const noexcept { return {q}; }
};

// RULE 3 supplies get_forward_progress_guarantee.
struct test_scheduler : compat::weakly_parallel_scheduler {
  using scheduler_concept = compat::scheduler_tag;
  run_queue* q = nullptr;
  [[nodiscard]] sched_sender schedule() const noexcept { return {q}; }
  bool operator==(const test_scheduler&) const noexcept = default;
};

inline test_scheduler sched_env::query(
    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
  return {{}, q};
}

// RULE 5: a real constructor, and copyable -- the shape the pipeline's
// receiver has.
struct handle_receiver {
  using receiver_concept = compat::receiver_tag;
  explicit handle_receiver(int* s) noexcept : sink(s) {}
  int* sink;
  void set_value(int v) && noexcept { *sink = v; }
  void set_value() && noexcept { *sink = -1; }
  void set_error(std::error_code) && noexcept { *sink = -2; }
  void set_error(std::exception_ptr) && noexcept { *sink = -3; }
  void set_stopped() && noexcept { *sink = -4; }
};

// The same, owning something move-only.
struct move_only_receiver {
  using receiver_concept = compat::receiver_tag;
  explicit move_only_receiver(int* s)
      : sink(s), owned(std::make_unique<int>(0)) {}
  int* sink;
  std::unique_ptr<int> owned;
  void set_value(int v) && noexcept { *sink = v; }
  void set_value() && noexcept { *sink = -1; }
  void set_error(std::error_code) && noexcept { *sink = -2; }
  void set_error(std::exception_ptr) && noexcept { *sink = -3; }
  void set_stopped() && noexcept { *sink = -4; }
};

// Every adaptor the pipeline uses, over one receiver type. Whether
// continues_on is included is a template argument, not a runtime flag: a
// provider that cannot compile it for this receiver must not see the
// call at all.
template <bool WithContinuesOn, class MakeReceiver>
void check_adaptors(MakeReceiver make) {
  {
    int v = 0;
    auto op = ex::connect(int_sender{7}, make(&v));
    ex::start(op);
    CHECK(v == 7);
  }
  {
    int v = 0;
    auto op = ex::connect(ex::then(int_sender{20}, [](int x) { return x + 1; }),
                          make(&v));
    ex::start(op);
    CHECK(v == 21);
  }
  {
    int v = 0;
    auto op = ex::connect(
        ex::let_value(int_sender{5}, [](int x) { return ex::just(x * 2); }),
        make(&v));
    ex::start(op);
    CHECK(v == 10);
  }
  {
    run_queue q;
    test_scheduler sch{{}, &q};
    int v = 0;
    auto op =
        ex::connect(ex::then(ex::schedule(sch), [] { return 3; }), make(&v));
    ex::start(op);
    while (q.drain_one()) {
    }
    CHECK(v == 3);
  }
  if constexpr (WithContinuesOn) {
    run_queue q;
    test_scheduler sch{{}, &q};
    int v = 0;
    auto op = ex::connect(ex::continues_on(int_sender{9}, sch), make(&v));
    ex::start(op);
    // Parked on the scheduler: nothing until the loop runs.
    CHECK(v == 0);
    while (q.drain_one()) {
    }
    CHECK(v == 9);
  }
}

// A receiver that carries a stop token, and a sender that reports what
// its own receiver's environment answers. Between them they ask the
// question the window chain depends on: does the token the scope hands
// down survive the adaptors above each stage?
//
// The discriminating case is a source that is ALREADY stopped. A token
// that was forwarded reports stop_requested(); one the adaptor replaced
// with never_stop_token reports false, which is also what a stage that
// silently never stops would do.
struct stop_env {
  ex::inplace_stop_token token;
  [[nodiscard]] ex::inplace_stop_token query(
      ex::get_stop_token_t) const noexcept {
    return token;
  }
};

struct stop_receiver {
  using receiver_concept = compat::receiver_tag;
  stop_receiver(int* s, ex::inplace_stop_token t) noexcept
      : sink(s), token(t) {}
  int* sink;
  ex::inplace_stop_token token;

  [[nodiscard]] stop_env get_env() const noexcept { return {token}; }
  void set_value(int v) && noexcept { *sink = v; }
  void set_value() && noexcept { *sink = -1; }
  void set_error(std::error_code) && noexcept { *sink = -2; }
  void set_error(std::exception_ptr) && noexcept { *sink = -3; }
  void set_stopped() && noexcept { *sink = -4; }
};

template <class Receiver>
struct token_probe_op {
  using operation_state_concept = compat::operation_state_tag;
  Receiver rcvr;
  bool* seen;
  void start() & noexcept {
    *seen = ex::get_stop_token(ex::get_env(rcvr)).stop_requested();
    ex::set_value(std::move(rcvr), 1);
  }
};

struct token_probe_sender {
  using sender_concept = compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t(int));
  bool* seen = nullptr;

  template <class Receiver>
  token_probe_op<Receiver> connect(Receiver r) const {
    return {std::move(r), seen};
  }
};

// What the pipeline's own receivers will be: a handle, copyable, with a
// real constructor. This one records the completion and takes the chain
// off the loop's outstanding count, which is what ends the run.
struct chain_result {
  bool completed = false;
  std::size_t bytes = 0;
  std::uint64_t sum = 0;
  std::error_code ec{};
};

struct chain_receiver {
  using receiver_concept = compat::receiver_tag;
  chain_receiver(chain_result* r, blake3pp::detail::loop_status* s) noexcept
      : out(r), status(s) {}
  chain_result* out;
  blake3pp::detail::loop_status* status;

  void finish() const noexcept {
    out->completed = true;
    if (status->outstanding > 0) {
      --status->outstanding;
    }
    status->done = status->outstanding == 0;
  }
  void set_value() && noexcept { finish(); }
  void set_value(std::span<const std::byte> w) && noexcept {
    out->bytes = w.size();
    finish();
  }
  void set_value(std::uint64_t v) && noexcept {
    out->sum = v;
    finish();
  }
  void set_error(std::error_code e) && noexcept {
    out->ec = e;
    finish();
  }
  void set_error(std::exception_ptr) && noexcept {
    out->ec = std::make_error_code(std::errc::state_not_recoverable);
    finish();
  }
  void set_stopped() && noexcept { finish(); }
};

[[nodiscard]] std::uint64_t byte_sum(std::span<const std::byte> w) noexcept {
  std::uint64_t s = 0;
  for (const std::byte b : w) {
    s += static_cast<std::uint64_t>(b);
  }
  return s;
}

namespace fs = std::filesystem;

std::vector<std::byte> pattern(std::size_t len, std::uint32_t seed) {
  std::vector<std::byte> v(len);
  std::uint32_t x = seed;
  for (auto& b : v) {
    x = x * 1'664'525u + 1'013'904'223u;
    b = static_cast<std::byte>(x >> 24);
  }
  return v;
}

struct temp_file {
  fs::path path;
  explicit temp_file(const std::vector<std::byte>& content) {
    static const unsigned run_id = std::random_device{}();
    path = fs::temp_directory_path() /
           ("blake3pp_pipe_test_" + std::to_string(run_id) + "_" +
            std::to_string(counter++));
    std::string chars(content.size(), '\0');
    std::ranges::transform(content, chars.begin(),
                           [](std::byte b) { return static_cast<char>(b); });
    std::ofstream out(path, std::ios::binary);
    out.write(chars.data(), static_cast<std::streamsize>(chars.size()));
  }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  static inline int counter = 0;
};

// A driver with no device under it: the file is a span of bytes, and the
// test decides in what order, in how many pieces and how successfully
// the reads finish. Everything the pipeline depends on that a real
// device would only produce by luck -- out-of-order completion, a read
// that takes several polls, a failure at one particular window -- is a
// parameter here.
struct fake_script {
  // Preference order by window index; a queued read whose index is not
  // named completes after every named one, in submission order.
  std::vector<unsigned> order{};
  // Bytes per completion; 0 delivers the whole window at once. A smaller
  // number makes a read take several polls, the way a short read does.
  std::size_t piece_bytes = 0;
  // The window index whose read fails, or -1 for none.
  int fail_at_window = -1;
  // The poll() call, counted from 0, that throws instead of serving a
  // read; -1 for none. With keep_throwing every later poll throws too.
  int throw_at_poll = -1;
  bool keep_throwing = false;
  // The window index whose submit_read() throws std::bad_alloc, or -1.
  int bad_alloc_at_window = -1;
  // What the device does about a cancel. All three are conforming, and
  // the pipeline has to report the same thing under each: the backends
  // differ exactly here (io_uring aborts, IOCP aborts, GCD has no
  // cancel at all, and any of them can lose the race to the data).
  enum cancel_mode {
    cancel_aborts,   // the read ends operation_canceled
    cancel_loses,    // the data had already arrived; the ask changes nothing
    cancel_ignored,  // the backend has no cancel mechanism
  };
  cancel_mode cancel_behaviour = cancel_aborts;
  // Polls between the ask and the cancelled completion, the way a real
  // ring takes a round or two to give the request back.
  unsigned cancel_after_polls = 0;
  // Requests stop through `stop_source` as window k's read is about to
  // be served, which is a stop arriving with windows in flight and the
  // driver in the middle of a round. -1 for none.
  int stop_at_window = -1;
  ex::inplace_stop_source* stop_source = nullptr;
  // Polls between that request and the failure at fail_at_window, for
  // the case where the two race.
  int fail_after_stop_polls = 0;
};

class fake_driver {
 public:
  using script = fake_script;

  fake_driver(std::span<const std::byte> content, std::size_t window_bytes,
              script s = {})
      : content_(content), window_bytes_(window_bytes), script_(std::move(s)) {}

  class file {
   public:
    explicit file(fake_driver& d) noexcept : drv_(&d) {}
    [[nodiscard]] std::uint64_t size() const noexcept {
      return drv_->content_.size();
    }
    [[nodiscard]] std::string_view name() const noexcept { return "fake"; }

   private:
    fake_driver* drv_;
  };

  void submit_read(file&, std::uint64_t off, std::span<std::byte> buf,
                   blake3pp::detail::io_read_op& op) {
    if (script_.bad_alloc_at_window >= 0 &&
        off / window_bytes_ ==
            static_cast<std::uint64_t>(script_.bad_alloc_at_window)) {
      throw std::bad_alloc();
    }
    queued_.push_back(read{&op, off, buf, 0, false, 0});
  }

  void flush() noexcept {}

  std::size_t poll(bool block) {
    const int nth = polls_++;
    if (script_.throw_at_poll >= 0 &&
        (nth == script_.throw_at_poll ||
         (script_.keep_throwing && nth > script_.throw_at_poll))) {
      throw std::runtime_error("poll failed");
    }
    if (queued_.empty()) {
      if (block) {
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return woken_; });
        woken_ = false;
      }
      return 0;
    }
    const std::size_t pick = choose();
    read& r = queued_[pick];
    if (script_.stop_at_window >= 0 && script_.stop_source != nullptr &&
        r.off / window_bytes_ ==
            static_cast<std::uint64_t>(script_.stop_at_window)) {
      script_.stop_source->request_stop();
      script_.stop_at_window = -1;  // once
      return 0;  // the stop lands before this read does
    }
    if (r.cancelled) {
      if (r.cancel_polls > 0) {
        --r.cancel_polls;
        return 0;  // asked for, not yet given back
      }
      blake3pp::detail::io_read_op* const op = r.op;
      queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
      ++callbacks_;
      op->done(op, std::make_error_code(std::errc::operation_canceled));
      return 1;
    }
    const unsigned index = static_cast<unsigned>(r.off / window_bytes_);
    if (script_.fail_at_window >= 0 &&
        index == static_cast<unsigned>(script_.fail_at_window)) {
      blake3pp::detail::io_read_op* const op = r.op;
      queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
      ++callbacks_;
      op->done(op, std::make_error_code(std::errc::io_error));
      return 1;
    }
    const std::size_t want = r.buf.size() - r.filled;
    const std::size_t take =
        script_.piece_bytes == 0 ? want : std::min(script_.piece_bytes, want);
    std::memcpy(r.buf.data() + r.filled,
                content_.data() + r.off + r.filled, take);
    r.filled += take;
    if (r.filled < r.buf.size()) {
      return 0;  // still owed: the read takes another poll
    }
    blake3pp::detail::io_read_op* const op = r.op;
    queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
    ++callbacks_;
    op->done(op, std::error_code{});
    return 1;
  }

  void wake() noexcept {
    {
      const std::lock_guard lock(m_);
      woken_ = true;
    }
    cv_.notify_one();
  }

  // What the script says this driver's device does about a cancel. The
  // three answers a real one can give are all here, because which of
  // them the pipeline gets must not change what it reports.
  void cancel(blake3pp::detail::io_read_op& op) noexcept {
    ++cancels_;
    for (read& r : queued_) {
      if (r.op == &op) {
        r.cancel_polls = script_.cancel_after_polls;
        r.cancelled = script_.cancel_behaviour != fake_script::cancel_ignored;
        return;
      }
    }
  }

  // How many times cancel() was called, so a test can check that the
  // sweep asked once per read rather than once per round.
  [[nodiscard]] unsigned cancels() const noexcept { return cancels_; }

  // Callbacks this driver has run, for the rule that none may run after
  // the pipeline has completed.
  [[nodiscard]] unsigned callbacks() const noexcept { return callbacks_; }

  [[nodiscard]] std::size_t in_flight() const noexcept {
    return queued_.size();
  }

  void drain() noexcept { queued_.clear(); }

  // A guard past the arena, filled with a byte the content never
  // contains. Nothing the pipeline does may reach it, so a read that
  // landed after the run was over, or one that overran its window, shows
  // up here even without a sanitizer.
  static constexpr std::size_t guard_bytes = 4096;
  static constexpr std::byte guard_fill{0xA5};

  void reserve(unsigned windows, std::size_t window_bytes) {
    arena_bytes_ = window_bytes * windows;
    window_stride_ = window_bytes;
    pool_.assign(arena_bytes_ + guard_bytes, guard_fill);
  }

  [[nodiscard]] blake3pp::detail::window_storage window_storage_for(
      file&, unsigned slot, std::uint64_t, std::size_t bytes) noexcept {
    const std::span<std::byte> slice{pool_.data() + slot * window_stride_,
                                     bytes};
    return {slice, std::span<const std::byte>(slice)};
  }

  [[nodiscard]] bool guard_intact() const noexcept {
    return std::all_of(pool_.begin() +
                           static_cast<std::ptrdiff_t>(arena_bytes_),
                       pool_.end(),
                       [](std::byte b) { return b == guard_fill; });
  }

  // What the scope asked for: the window objects it allocated, in bytes.
  [[nodiscard]] std::size_t pool_bytes() const noexcept {
    return arena_bytes_;
  }

 private:
  struct read {
    blake3pp::detail::io_read_op* op;
    std::uint64_t off;
    std::span<std::byte> buf;
    std::size_t filled;
    bool cancelled = false;
    unsigned cancel_polls = 0;
  };

  // The queued read the script prefers: lowest rank wins, and a read the
  // script does not name ranks after every one it does.
  [[nodiscard]] std::size_t choose() const noexcept {
    std::size_t best = 0;
    std::size_t best_rank = rank_of(queued_.front());
    for (std::size_t i = 1; i < queued_.size(); ++i) {
      const std::size_t r = rank_of(queued_[i]);
      if (r < best_rank) {
        best = i;
        best_rank = r;
      }
    }
    return best;
  }

  [[nodiscard]] std::size_t rank_of(const read& r) const noexcept {
    const auto index = static_cast<unsigned>(r.off / window_bytes_);
    for (std::size_t i = 0; i < script_.order.size(); ++i) {
      if (script_.order[i] == index) {
        return i;
      }
    }
    return script_.order.size() + index;
  }

  std::span<const std::byte> content_;
  std::size_t window_bytes_;
  script script_;
  std::vector<read> queued_;
  std::vector<std::byte> pool_;
  std::mutex m_;
  std::condition_variable cv_;
  bool woken_ = false;
  int polls_ = 0;
  unsigned cancels_ = 0;
  unsigned callbacks_ = 0;
  std::size_t arena_bytes_ = 0;
  std::size_t window_stride_ = 0;
};

static_assert(blake3pp::detail::file_driver<fake_driver>);

// Counts completions and remembers the error, like the backend probes.
struct read_probe {
  int calls = 0;
  std::error_code ec{};
  static void on_done(blake3pp::detail::io_read_op* op,
                      std::error_code e) noexcept {
    auto* const p = static_cast<read_probe*>(op->owner);
    p->calls++;
    p->ec = e;
  }
};

}  // namespace

TEST_SUITE("file_pipeline") {

TEST_CASE("the provider hosts custom senders and schedulers") {
  MESSAGE("execution provider: " << provider_name());
  static_assert(ex::sender<int_sender>);
  static_assert(ex::scheduler<test_scheduler>);
  static_assert(ex::sender<sched_sender>);
  CHECK(ex::sender<int_sender>);
  CHECK(ex::scheduler<test_scheduler>);
}

TEST_CASE("the adaptors compose with a copyable handle receiver") {
  check_adaptors<true>([](int* v) { return handle_receiver{v}; });
}

TEST_CASE("the adaptors compose with a move-only receiver") {
  // beman's continues_on stores the receiver by copy from an lvalue
  // (continues_on.hpp, state_type), so a move-only receiver cannot pass
  // through it there. Every other adaptor takes one on every provider.
  // This is why the pipeline's receivers are copyable handles.
#if defined(BLAKE3PP_EXECUTION_BEMAN)
  MESSAGE("beman: continues_on copies the receiver, so it is skipped here");
  check_adaptors<false>([](int* v) { return move_only_receiver{v}; });
  static_assert(!std::copy_constructible<move_only_receiver>);
#else
  check_adaptors<true>([](int* v) { return move_only_receiver{v}; });
#endif
}

// The window chain is read | let_value(compress) | continues_on | then,
// and every stage below the scope has to see the scope's token: the read
// so a cancelled completion can be told from a failed one, the compress
// stage so it stops claiming parts. The adaptors in between are the only
// thing that could drop it, and whether they do is provider business.
TEST_CASE("the adaptors forward a stop token down the window chain") {
  ex::inplace_stop_source source;
  source.request_stop();

  bool at_head = false;
  bool in_let_value = false;
  run_queue q;
  test_scheduler sch{{}, &q};
  int v = 0;

  auto chain = ex::then(
      ex::continues_on(
          ex::let_value(token_probe_sender{&at_head},
                        [&](int) { return token_probe_sender{&in_let_value}; }),
          sch),
      [](int x) { return x + 40; });

  auto op = ex::connect(std::move(chain), stop_receiver{&v, source.get_token()});
  ex::start(op);
  while (q.drain_one()) {
  }

  CHECK(at_head);
  CHECK(in_let_value);
  CHECK(v == 41);
}

// The same chain over a source nobody stopped: the token is live, so the
// probes must report false rather than throwing or refusing to compile.
TEST_CASE("a forwarded stop token that was never requested reads false") {
  ex::inplace_stop_source source;

  bool at_head = true;
  bool in_let_value = true;
  run_queue q;
  test_scheduler sch{{}, &q};
  int v = 0;

  auto chain = ex::then(
      ex::continues_on(
          ex::let_value(token_probe_sender{&at_head},
                        [&](int) { return token_probe_sender{&in_let_value}; }),
          sch),
      [](int x) { return x + 40; });

  auto op = ex::connect(std::move(chain), stop_receiver{&v, source.get_token()});
  ex::start(op);
  while (q.drain_one()) {
  }

  CHECK(!at_head);
  CHECK(!in_let_value);
  CHECK(v == 41);
}

// The std::execution path has never been built or run: no toolchain in
// the matrix has <execution> senders. If one gains them, this fails
// until a preset selects BLAKE3PP_EXECUTION_PROVIDER=std and the cases
// above are run against it, so the std path cannot go live untested.
TEST_CASE("std::execution senders are not silently untested") {
#if defined(BLAKE3PP_HAS_STD_SENDERS) && !defined(BLAKE3PP_EXECUTION_STD)
  FAIL("this toolchain has std::execution senders, but the conformance "
       "cases were built against "
       << provider_name()
       << ". Configure a preset with BLAKE3PP_EXECUTION_PROVIDER=std and "
          "run these cases there before shipping the std path.");
#else
  MESSAGE("std::execution senders unavailable here; conformance covers "
          << provider_name());
  CHECK(true);
#endif
}

// io_read_op::storage is sized for the widest backend, but only the
// platform being built sees its own. Reporting the margin here is how a
// backend that grows towards the limit becomes visible before the
// static_assert in io_driver.cpp stops the build.
TEST_CASE("the backend's read operation fits the driver's op storage") {
  using blake3pp::detail::io_read_op;
  const auto layout = blake3pp::detail::native_read_op_layout();
  MESSAGE("backend read_op: " << layout.size << " bytes, align "
                              << layout.align << " (storage "
                              << io_read_op::storage_size << " / "
                              << io_read_op::storage_align << ")");
  CHECK(layout.size <= io_read_op::storage_size);
  CHECK(layout.align <= io_read_op::storage_align);
}

// The compiled seam: the same contract the backends keep, from behind a
// pimpl, plus the arena whose lifetime the drain depends on.
TEST_CASE("the io driver reads a file and keeps the callback discipline") {
  using blake3pp::detail::io_driver;
  using blake3pp::detail::io_read_op;
  constexpr std::size_t len = 256 * 1024;
  const auto content = pattern(len, 13);
  const temp_file f(content);

  io_driver drv({}, 4);
  io_driver::file file(drv, f.path, /*direct_io=*/false);
  CHECK(file.size() == len);
  CHECK(!file.name().empty());
  MESSAGE("driver backend: " << file.name());

  // The arena is the driver's, direct-I/O aligned, and outlives every
  // read because the driver drains before freeing it.
  drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;
  REQUIRE(pool.size() == len);
  CHECK(std::bit_cast<std::uintptr_t>(pool.data()) % 4096 == 0);

  io_read_op op{};
  read_probe probe;
  op.done = &read_probe::on_done;
  op.owner = &probe;
  drv.submit_read(file, 0, pool, op);
  CHECK(probe.calls == 0);
  drv.flush();
  CHECK(probe.calls == 0);        // never from submit or flush
  CHECK(drv.in_flight() == 1);

  std::size_t ran = 0;
  while (probe.calls == 0) {
    ran += drv.poll(true);
  }
  CHECK(ran == 1);
  CHECK(probe.calls == 1);
  CHECK(!probe.ec);
  CHECK(drv.in_flight() == 0);
  CHECK(std::equal(pool.begin(), pool.end(), content.begin()));
}

// The teardown contract the arena exists for: a driver destroyed with a
// read still owed drains it, running no callback, before the memory that
// read targets is freed. The sanitizer lanes are what would object.
TEST_CASE("the io driver drains a read in flight before freeing its arena") {
  using blake3pp::detail::io_driver;
  using blake3pp::detail::io_read_op;
  constexpr std::size_t len = 256 * 1024;
  const auto content = pattern(len, 17);
  const temp_file f(content);

  read_probe probe;
  {
    io_driver drv({}, 4);
    io_driver::file file(drv, f.path, /*direct_io=*/false);
    drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;
    io_read_op op{};
    op.done = &read_probe::on_done;
    op.owner = &probe;
    drv.submit_read(file, 0, pool, op);
    drv.flush();
    CHECK(drv.in_flight() == 1);
  }
  CHECK(probe.calls == 0);
}

// The same drain on a live driver: the owed read is reaped and never
// reported, the op may go, and the driver then serves another read.
TEST_CASE("the io driver drains on request and reads again afterwards") {
  using blake3pp::detail::io_driver;
  using blake3pp::detail::io_read_op;
  constexpr std::size_t len = 256 * 1024;
  const auto content = pattern(len, 19);
  const temp_file f(content);

  io_driver drv({}, 4);
  io_driver::file file(drv, f.path, /*direct_io=*/false);
  drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;
  read_probe probe;
  {
    io_read_op op{};
    op.done = &read_probe::on_done;
    op.owner = &probe;
    drv.submit_read(file, 0, pool, op);
    drv.flush();
    drv.drain();
    CHECK(drv.in_flight() == 0);
    CHECK(probe.calls == 0);
  }
  io_read_op op{};
  op.done = &read_probe::on_done;
  op.owner = &probe;
  drv.submit_read(file, 0, pool, op);
  while (probe.calls == 0) {
    drv.poll(true);
  }
  CHECK(probe.calls == 1);
  CHECK(!probe.ec);
  CHECK(std::equal(pool.begin(), pool.end(), content.begin()));
}

TEST_CASE("the io driver wakes a blocked poll from another thread") {
  using blake3pp::detail::io_driver;
  io_driver drv({}, 4);
  std::atomic<bool> entered{false};
  std::size_t ran = 1;
  std::thread driver([&] {
    entered.store(true, std::memory_order_release);
    ran = drv.poll(true);
  });
  while (!entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  drv.wake();
  driver.join();
  CHECK(ran == 0);
}

// --------------------------------------------------------------------
// Section 3: the read as a sender, the driver as a scheduler, and the
// loop that drives both.

TEST_CASE("a window read is a sender the driver loop completes") {
  using blake3pp::detail::driver_loop;
  using blake3pp::detail::io_driver;
  constexpr std::size_t len = 128 * 1024;
  const auto content = pattern(len, 21);
  const temp_file f(content);

  io_driver drv({}, 4);
  driver_loop<io_driver> io(drv);
  io_driver::file file(drv, f.path, /*direct_io=*/false);
  drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;

  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  blake3pp::detail::io_read_op rop{};
  auto op = ex::connect(io.read(file, 0, pool, rop), chain_receiver{&r, &st});
  // start() queues the read and nothing else: no flush, no callback.
  ex::start(op);
  CHECK(!r.completed);
  CHECK(drv.in_flight() == 1);

  io.run_until(st);
  CHECK(r.completed);
  CHECK(!r.ec);
  CHECK(r.bytes == len);
  CHECK(std::equal(pool.begin(), pool.end(), content.begin()));
}

TEST_CASE("a read that fails reports through the error channel") {
  using blake3pp::detail::driver_loop;
  using blake3pp::detail::io_driver;
  constexpr std::size_t len = 64 * 1024;
  const auto content = pattern(len, 5);
  const temp_file f(content);

  io_driver drv({}, 4);
  driver_loop<io_driver> io(drv);
  io_driver::file file(drv, f.path, /*direct_io=*/false);
  drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;

  // Past the end of the file: the backend reports the unexpected EOF as
  // an error, and the chain sees it as std::error_code, not a value.
  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  blake3pp::detail::io_read_op rop{};
  auto op = ex::connect(io.read(file, len, pool, rop), chain_receiver{&r, &st});
  ex::start(op);
  io.run_until(st);
  CHECK(r.completed);
  CHECK(r.ec);
  CHECK(r.bytes == 0);
}

TEST_CASE("the driver scheduler runs work on the loop's own thread") {
  using blake3pp::detail::driver_loop;
  using blake3pp::detail::io_driver;
  io_driver drv({}, 4);
  driver_loop<io_driver> io(drv);

  const auto here = std::this_thread::get_id();
  std::thread::id ran_on{};
  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  auto op = ex::connect(
      ex::then(ex::schedule(io.scheduler()),
               [&] { ran_on = std::this_thread::get_id(); }),
      chain_receiver{&r, &st});
  // Parked on the run queue: nothing happens until the loop drains it.
  ex::start(op);
  CHECK(!r.completed);
  io.run_until(st);
  CHECK(r.completed);
  CHECK(ran_on == here);
}

TEST_CASE("work parked from another thread wakes a blocked driver") {
  using blake3pp::detail::driver_loop;
  using blake3pp::detail::io_driver;
  constexpr std::size_t len = 64 * 1024;
  const auto content = pattern(len, 9);
  const temp_file f(content);

  io_driver drv({}, 4);
  driver_loop<io_driver> io(drv);
  io_driver::file file(drv, f.path, /*direct_io=*/false);
  drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;

  // Two chains, so the loop runs until both are in: the read is what
  // makes blocking legitimate, and the other thread's schedule is what
  // has to get through to a driver already asleep.
  blake3pp::detail::loop_status st{false, 2};
  chain_result read_done;
  blake3pp::detail::io_read_op rop{};
  auto read_op = ex::connect(io.read(file, 0, pool, rop),
                             chain_receiver{&read_done, &st});
  chain_result parked;
  auto sched_op =
      ex::connect(ex::schedule(io.scheduler()), chain_receiver{&parked, &st});

  std::atomic<bool> go{false};
  std::thread other([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    ex::start(sched_op);  // push and wake, from off the driver thread
  });

  ex::start(read_op);
  go.store(true, std::memory_order_release);
  io.run_until(st);
  other.join();

  CHECK(read_done.completed);
  CHECK(parked.completed);
  CHECK(!read_done.ec);
  CHECK(std::equal(pool.begin(), pool.end(), content.begin()));
}

TEST_CASE("the four-line window chain composes on this provider") {
  using blake3pp::detail::driver_loop;
  using blake3pp::detail::io_driver;
  constexpr std::size_t len = 256 * 1024;
  const auto content = pattern(len, 77);
  const temp_file f(content);

  io_driver drv({}, 4);
  driver_loop<io_driver> io(drv);
  io_driver::file file(drv, f.path, /*direct_io=*/false);
  drv.reserve(1, len);
  const auto pool = drv.window_storage_for(file, 0, 0, len).read_into;

  // The shape of the real window chain, with a stand-in for the compress
  // stage: read on the driver, work on the pool, back on the driver.
  // Only the middle line differs from the real thing, so this is the
  // case that says whether the shape itself holds on this provider.
  auto sched = blake3pp::get_parallel_scheduler();
  const auto here = std::this_thread::get_id();
  std::atomic<bool> summed_elsewhere{false};
  std::thread::id returned_on{};

  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  blake3pp::detail::io_read_op rop{};
  auto op = ex::connect(
      ex::then(
          ex::continues_on(
              ex::let_value(io.read(file, 0, pool, rop),
                            [&, sched](std::span<const std::byte> w) {
                              return ex::then(ex::schedule(sched), [&, w] {
                                summed_elsewhere.store(
                                    std::this_thread::get_id() != here,
                                    std::memory_order_relaxed);
                                return byte_sum(w);
                              });
                            }),
              io.scheduler()),
          [&](std::uint64_t v) {
            returned_on = std::this_thread::get_id();
            return v;
          }),
      chain_receiver{&r, &st});
  ex::start(op);
  io.run_until(st);

  CHECK(r.completed);
  CHECK(!r.ec);
  CHECK(r.sum == byte_sum(std::span<const std::byte>(content)));
  // Whatever thread the pool picked, continues_on brought the result
  // back to this one: that is what lets the reducer stay single-threaded.
  CHECK(returned_on == here);
  MESSAGE("compress stage left the driver thread: "
          << summed_elsewhere.load(std::memory_order_relaxed));
}

// --------------------------------------------------------------------
// Section 5: the bounded scope.
//
// The pipeline absorbs windows in whatever order they finish, so the one
// thing every case checks is the digest: any mistake in the geometry, the
// reducer or the last window shows up there and nowhere else.

namespace {

// Runs one file through the pipeline and finalizes.
[[nodiscard]] blake3pp::digest pipeline_digest(const fs::path& path,
                                               std::size_t window,
                                               unsigned depth, unsigned cap) {
  using blake3pp::detail::io_driver;
  auto sched = blake3pp::get_parallel_scheduler();
  io_driver drv({/*async=*/true, /*offload_submit=*/true}, depth);
  io_driver::file f(drv, path, /*direct_io=*/false);
  blake3pp::hasher h;
  blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
      h, drv, f, sched, {window, depth, nullptr, cap});
  return h.finalize();
}

}  // namespace

TEST_CASE("the pipeline's digest matches the sequential hash") {
  constexpr std::size_t win = 64 * 1024;
  // Empty, under one window, exactly one window, an exact multiple, and
  // a multiple plus a tail: every shape the last window can take.
  for (const std::size_t len :
       {std::size_t{0}, win / 2, win, 4 * win, 4 * win + 4097}) {
    const auto content = pattern(len, static_cast<std::uint32_t>(len + 1));
    const temp_file f(content);
    const auto expected = blake3pp::hash(content);
    // One window in flight is the in-order case: every insert arrives
    // where the reducer would have put it anyway. The full count is what
    // lets windows finish out of order.
    for (const unsigned cap : {1u, 0u}) {
      for (const unsigned depth : {2u, 4u, 32u}) {
        CAPTURE(len);
        CAPTURE(cap);
        CAPTURE(depth);
        CHECK(pipeline_digest(f.path, win, depth, cap) == expected);
      }
    }
  }
}

TEST_CASE("the pipeline's digest is independent of the window size") {
  const std::size_t len = 3 * 1024 * 1024 + 12345;
  const auto content = pattern(len, 3);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);
  for (const std::size_t win :
       {std::size_t{64} * 1024, std::size_t{256} * 1024,
        std::size_t{1} << 20}) {
    CAPTURE(win);
    CHECK(pipeline_digest(f.path, win, 4, 0) == expected);
    CHECK(pipeline_digest(f.path, win, 4, 1) == expected);
  }
}

TEST_CASE("update_file over a scheduler takes the pipeline and agrees") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::size_t len = 5 * 64 * 1024 + 77;
  const auto content = pattern(len, 19);
  const temp_file f(content);

  blake3pp::hasher h;
  blake3pp::update_file(h, f.path, sched, {.window_bytes = 64 * 1024});
  CHECK(h.finalize() == blake3pp::hash(content));

  // A hasher that is not on a window boundary cannot absorb windows as
  // subtrees; it takes the sequential path and must still agree.
  blake3pp::hasher part;
  part.update(std::span(content).first(1000));
  blake3pp::update_file(part, f.path, sched, {.window_bytes = 64 * 1024});
  blake3pp::hasher ref;
  ref.update(std::span(content).first(1000));
  ref.update(content);
  CHECK(part.finalize() == ref.finalize());
}

// Runs the pipeline over the fake driver and returns the scope, so a
// test can read what admission did.
template <class Scope>
void run_scope(Scope& scope) {
  scope.run();
}

// A scope and the pool it borrows windows from, paired the way a
// blocking entry point pairs them: the pool is sized from the same plan
// the scope runs.
template <class Scope, class D, class Sched>
struct pooled_scope {
  pooled_scope(blake3pp::hasher& h, D& drv, typename D::file& f, Sched sched,
               const blake3pp::detail::pipeline_options& opts)
      : plan(blake3pp::detail::plan_pipeline(h.count(), opts.window_bytes,
                                             opts.queue_depth, false,
                                             f.size())),
        host(drv, plan.windows, plan.window_bytes),
        scope(h, host, f, std::move(sched), plan, opts) {}

  blake3pp::detail::pipeline_plan plan;
  blake3pp::detail::pipeline_host<blake3pp::default_stack_budget, D> host;
  Scope scope;
};

TEST_CASE("sibling windows the reducer cannot hold still finish") {
  // The liveness case admission control exists for. Were the reducer to
  // refuse inserts and the scope retry them, two windows refused one
  // after the other could never merge: the second looks for its sibling
  // inside the reducer, and the first is held outside it. Every window
  // ends up held, nothing is in flight and the run never finishes.
  using blake3pp::default_stack_budget;
  using scope_type =
      blake3pp::detail::window_scope<false, default_stack_budget, fake_driver,
                                     blake3pp::parallel_scheduler_t>;
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 61);

  fake_driver drv(content, win, {.order = {0, 4, 2, 3, 1, 5}});
  fake_driver::file f(drv);
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  // Eight nodes is the smallest capacity this file may legally run with.
  // A contiguous run of k of its eight windows holds popcount(k) nodes,
  // three at k = 7, and the last window is an edge window that reserves
  // the decomposition of its own three parts, four more. Below the sum
  // the scope stalls by construction, which is what the assert in the
  // start hook says.
  pooled_scope<scope_type, fake_driver, decltype(sched)> ps(h, drv, f, sched,
                   {.window_bytes = win,
                    .queue_depth = 8,
                    .trace = nullptr,
                    .in_flight_cap = 0,
                    .reducer_capacity = 8});
  run_scope(ps.scope);
  CHECK(h.finalize() == blake3pp::hash(content));
}

TEST_CASE("admission throttles starts instead of refusing an insert") {
  using blake3pp::default_stack_budget;
  using scope_type =
      blake3pp::detail::window_scope<false, default_stack_budget, fake_driver,
                                     blake3pp::parallel_scheduler_t>;
  constexpr std::size_t win = 64 * 1024;
  // Eight windows: a contiguous run of k of them holds popcount(k)
  // nodes, three at most, and the last window reserves four for its own
  // parts. Eight is just above that sum and far below what eight windows
  // starting at once would want.
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 62);

  fake_driver drv(content, win, {.order = {3, 1, 0, 2}});
  fake_driver::file f(drv);
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  pooled_scope<scope_type, fake_driver, decltype(sched)> ps(h, drv, f, sched,
                   {.window_bytes = win,
                    .queue_depth = 8,
                    .trace = nullptr,
                    .in_flight_cap = 0,
                    .reducer_capacity = 8});
  run_scope(ps.scope);
  CHECK(h.finalize() == blake3pp::hash(content));
  // The scope held windows back, so the reducer refused none.
  CHECK(ps.scope.admission_stalls() > 0);
}

TEST_CASE("the full reducer capacity never throttles a normal run") {
  using blake3pp::default_stack_budget;
  using scope_type =
      blake3pp::detail::window_scope<false, default_stack_budget, fake_driver,
                                     blake3pp::parallel_scheduler_t>;
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 32 * win + 999;
  const auto content = pattern(len, 63);

  fake_driver drv(content, win, {.order = {5, 2, 9, 0, 7}});
  fake_driver::file f(drv);
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  pooled_scope<scope_type, fake_driver, decltype(sched)> ps(h, drv, f, sched,
                   {.window_bytes = win, .queue_depth = 8});
  run_scope(ps.scope);
  CHECK(h.finalize() == blake3pp::hash(content));
  CHECK(ps.scope.admission_stalls() == 0);
}

// Runs a file through the pipeline over the fake driver, with whatever
// hasher and scheduler the case wants.
template <class Sched>
void fake_run(blake3pp::hasher& h, std::span<const std::byte> content,
              std::size_t win, Sched sched, unsigned depth = 4,
              fake_script script = {}, unsigned cap = 0) {
  fake_driver drv(content, win, std::move(script));
  fake_driver::file f(drv);
  blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
      h, drv, f, std::move(sched),
      {.window_bytes = win, .queue_depth = depth, .in_flight_cap = cap});
}

// The stop cases all want the driver afterwards -- how many reads it was
// asked to cancel, how many callbacks it ran -- so they drive the scope
// themselves rather than through fake_run.
struct stop_outcome {
  bool threw = false;
  std::error_code ec{};
  unsigned cancels = 0;
  unsigned callbacks_at_return = 0;
  unsigned callbacks_after = 0;
  std::size_t still_queued = 0;
  bool guard_intact = true;
};

template <class Sched>
stop_outcome stop_run(blake3pp::hasher& h, std::span<const std::byte> content,
                      std::size_t win, Sched sched,
                      ex::inplace_stop_source& src, fake_script script,
                      unsigned depth = 4, unsigned cap = 0) {
  script.stop_source = &src;
  stop_outcome out;
  fake_driver drv(content, win, std::move(script));
  fake_driver::file f(drv);
  try {
    blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
        h, drv, f, std::move(sched),
        {.window_bytes = win,
         .queue_depth = depth,
         .in_flight_cap = cap,
         .stop_token = src.get_token()});
  } catch (const std::system_error& e) {
    out.threw = true;
    out.ec = e.code();
  }
  out.cancels = drv.cancels();
  out.callbacks_at_return = drv.callbacks();
  out.still_queued = drv.in_flight();
  // Nothing the pipeline owns may still be reached: every read it was
  // owed has reported, so a further poll has nothing to run.
  drv.poll(false);
  out.callbacks_after = drv.callbacks() - out.callbacks_at_return;
  out.guard_intact = drv.guard_intact();
  return out;
}

TEST_CASE("an external stop mid-file ends the run once, as cancelled") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 12 * win + 777;
  const auto content = pattern(len, 91);

  // All three answers a device can give about a cancel. The pipeline has
  // to report the same thing under each.
  for (const auto mode : {fake_script::cancel_aborts, fake_script::cancel_loses,
                          fake_script::cancel_ignored}) {
    CAPTURE(static_cast<int>(mode));
    ex::inplace_stop_source src;
    blake3pp::hasher h;
    const stop_outcome out = stop_run(
        h, content, win, sched, src,
        {.cancel_behaviour = mode, .cancel_after_polls = 1,
         .stop_at_window = 3});

    CHECK(out.threw);
    CHECK(out.ec == std::errc::operation_canceled);
    CHECK(out.still_queued == 0);
    CHECK(out.callbacks_after == 0);
    CHECK(out.guard_intact);
  }
}

TEST_CASE("the sweep asks the device once per read it has not landed") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 12 * win;
  const auto content = pattern(len, 92);
  ex::inplace_stop_source src;
  blake3pp::hasher h;
  // Reads take two polls to come back, so several are certainly still
  // reading when the stop lands.
  const stop_outcome out =
      stop_run(h, content, win, sched, src,
               {.cancel_after_polls = 2, .stop_at_window = 2});
  CHECK(out.threw);
  CHECK(out.ec == std::errc::operation_canceled);
  // At least the one the driver was about to serve, and never more than
  // the reads the plan allows in flight.
  CHECK(out.cancels >= 1);
  CHECK(out.cancels <= 4);
  CHECK(out.still_queued == 0);
  CHECK(out.callbacks_after == 0);
  CHECK(out.guard_intact);
}

TEST_CASE("a failure outranks the stop it caused") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 12 * win + 31;
  const auto content = pattern(len, 93);
  ex::inplace_stop_source src;
  blake3pp::hasher h;
  // The read at window 2 fails; the external stop lands on window 3,
  // which the driver serves after it. The caller hears the failure.
  const stop_outcome out = stop_run(
      h, content, win, sched, src,
      {.fail_at_window = 2, .cancel_after_polls = 1, .stop_at_window = 3});
  CHECK(out.threw);
  CHECK(out.ec == std::errc::io_error);
  CHECK(out.still_queued == 0);
  CHECK(out.callbacks_after == 0);
  CHECK(out.guard_intact);
}

TEST_CASE("a stop requested before the run submits no read at all") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 6 * win;
  const auto content = pattern(len, 94);
  ex::inplace_stop_source src;
  src.request_stop();
  blake3pp::hasher h;
  const stop_outcome out = stop_run(h, content, win, sched, src, {});
  CHECK(out.threw);
  CHECK(out.ec == std::errc::operation_canceled);
  CHECK(out.callbacks_at_return == 0);
  CHECK(out.cancels == 0);
  CHECK(out.still_queued == 0);
}

TEST_CASE("a stop lands on the last window and on one window in flight") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;

  SUBCASE("the last window, which carries the message end") {
    // Six windows and a short tail: the stop arrives as the last one is
    // served, so the edge chain is what sees it.
    const std::size_t len = 6 * win + 4321;
    const auto content = pattern(len, 95);
    ex::inplace_stop_source src;
    blake3pp::hasher h;
    const stop_outcome out =
        stop_run(h, content, win, sched, src, {.stop_at_window = 6});
    CHECK(out.threw);
    CHECK(out.ec == std::errc::operation_canceled);
    CHECK(out.still_queued == 0);
    CHECK(out.callbacks_after == 0);
    CHECK(out.guard_intact);
  }

  SUBCASE("one window in flight at a time") {
    const std::size_t len = 8 * win;
    const auto content = pattern(len, 96);
    ex::inplace_stop_source src;
    blake3pp::hasher h;
    const stop_outcome out = stop_run(h, content, win, sched, src,
                                      {.stop_at_window = 3}, 4, /*cap=*/1);
    CHECK(out.threw);
    CHECK(out.ec == std::errc::operation_canceled);
    CHECK(out.still_queued == 0);
    CHECK(out.callbacks_after == 0);
    CHECK(out.guard_intact);
  }
}

TEST_CASE("a stop ends the run on an inline scheduler and on one thread") {
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 10 * win + 5;
  const auto content = pattern(len, 97);

  SUBCASE("inline") {
    ex::inplace_stop_source src;
    blake3pp::hasher h;
    const stop_outcome out = stop_run(h, content, win, b3test::inline_scheduler{}, src,
                                      {.stop_at_window = 4});
    CHECK(out.threw);
    CHECK(out.ec == std::errc::operation_canceled);
    CHECK(out.still_queued == 0);
    CHECK(out.callbacks_after == 0);
    CHECK(out.guard_intact);
  }
  SUBCASE("one thread") {
    b3test::one_thread_pool pool;
    ex::inplace_stop_source src;
    blake3pp::hasher h;
    const stop_outcome out = stop_run(h, content, win, pool.get_scheduler(),
                                      src, {.stop_at_window = 4});
    CHECK(out.threw);
    CHECK(out.ec == std::errc::operation_canceled);
    CHECK(out.still_queued == 0);
    CHECK(out.callbacks_after == 0);
    CHECK(out.guard_intact);
  }
}

TEST_CASE("a stop requested from another thread reaches a parked driver") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 64 * win;
  const auto content = pattern(len, 98);
  ex::inplace_stop_source src;
  blake3pp::hasher h;
  // Nothing in the driver's own round requests this one: another thread
  // does, which is the path that needs wake() to break a blocking poll.
  std::thread stopper([&src] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    src.request_stop();
  });
  fake_driver drv(content, win, {.cancel_after_polls = 1});
  fake_driver::file f(drv);
  bool threw = false;
  std::error_code ec;
  try {
    blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
        h, drv, f, sched,
        {.window_bytes = win, .queue_depth = 4,
         .stop_token = src.get_token()});
  } catch (const std::system_error& e) {
    threw = true;
    ec = e.code();
  }
  stopper.join();
  // Either it finished the file before the stop arrived or it stopped;
  // both are correct, and neither may leave a read owed.
  if (threw) {
    CHECK(ec == std::errc::operation_canceled);
  }
  CHECK(drv.in_flight() == 0);
}

// The blocking entry points take no stop token -- async_io.hpp is where
// a caller brings one -- so the only way into their stop path is the
// seam the test target compiles.
// What it has to prove is the mapping and the aftermath: one
// operation_canceled, and a hasher that is still a hasher.
// The real driver, with reads genuinely on the ring. The buffer arena
// belongs to the io_driver and is freed when update_file() returns, so a
// read that landed after the pipeline completed is a write into freed
// memory -- which is what the sanitizer lanes are here to catch, and a
// stricter canary than any pattern the test could write itself.
//
// Whether a given attempt stops mid-file is a race with the device, so
// this runs several and reports how many landed; every one of them, stop
// or no stop, has to leave the driver owing nothing.
TEST_CASE("a real driver stops mid-file with reads in flight") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 48 * win + 4095;
  const auto content = pattern(len, 100);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);

  int stopped = 0;
  constexpr int attempts = 8;
  for (int i = 0; i < attempts; ++i) {
    ex::inplace_stop_source src;
    blake3pp::detail::installed_stop_source() = &src;
    blake3pp::hasher h;
    std::error_code ec;
    // Requested from another thread while the driver is inside poll():
    // the path that needs wake() to break a blocking poll, and the one
    // ThreadSanitizer has something to say about.
    std::thread stopper([&src, i] {
      std::this_thread::sleep_for(std::chrono::microseconds(50 * i));
      src.request_stop();
    });
    blake3pp::update_file(h, f.path, sched, ec, {.window_bytes = win});
    stopper.join();
    blake3pp::detail::installed_stop_source() = nullptr;
    if (ec) {
      CHECK(ec == std::errc::operation_canceled);
      ++stopped;
    } else {
      CHECK(h.finalize() == expected);
    }
  }
  MESSAGE("stopped mid-file on " << stopped << " of " << attempts
                                 << " attempts");
  // The file is large enough that at least one of a spread of delays has
  // to land inside it; a run where none did has not tested anything.
  CHECK(stopped > 0);
}

TEST_CASE("a stopped update_file reports operation_canceled") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 20 * win + 909;
  const auto content = pattern(len, 99);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);

  SUBCASE("stopped before it starts, so the hasher is untouched") {
    ex::inplace_stop_source src;
    src.request_stop();
    blake3pp::detail::installed_stop_source() = &src;
    blake3pp::hasher h;
    std::error_code ec;
    CHECK_THROWS_AS(
        blake3pp::update_file(h, f.path, sched, {.window_bytes = win}),
        std::system_error);
    try {
      blake3pp::update_file(h, f.path, sched, {.window_bytes = win});
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    CHECK(ec == std::errc::operation_canceled);

    // Nothing was read, so the same hasher still hashes the file to the
    // digest a fresh one would produce.
    blake3pp::detail::installed_stop_source() = nullptr;
    blake3pp::update_file(h, f.path, sched, {.window_bytes = win});
    CHECK(h.finalize() == expected);
  }

  SUBCASE("the error_code overload reports it instead of throwing") {
    ex::inplace_stop_source src;
    src.request_stop();
    blake3pp::detail::installed_stop_source() = &src;
    blake3pp::hasher h;
    std::error_code ec;
    blake3pp::update_file(h, f.path, sched, ec, {.window_bytes = win});
    CHECK(ec == std::errc::operation_canceled);
    blake3pp::detail::installed_stop_source() = nullptr;
  }

  SUBCASE("stopped part-way, and the hasher still works afterwards") {
    ex::inplace_stop_source src;
    blake3pp::detail::installed_stop_source() = &src;
    blake3pp::hasher h;
    std::thread stopper([&src] { src.request_stop(); });
    std::error_code ec;
    blake3pp::update_file(h, f.path, sched, ec, {.window_bytes = win});
    stopper.join();
    blake3pp::detail::installed_stop_source() = nullptr;
    // A race: the file is small enough that it may finish first. Either
    // outcome is correct, and neither may leave the hasher broken.
    if (ec) {
      CHECK(ec == std::errc::operation_canceled);
    }
    // Whatever it absorbed, it is still a hasher: it takes more input
    // and finalizes, and a fresh one over the same file is unaffected.
    h.update(content);
    (void)h.finalize();
    blake3pp::hasher again;
    blake3pp::update_file(again, f.path, sched, {.window_bytes = win});
    CHECK(again.finalize() == expected);
  }
}

TEST_CASE("the pipeline matches the sequential hash in every mode") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 6 * win + 1234;
  const auto content = pattern(len, 71);
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i * 7 + 1);
  }

  SUBCASE("plain") {
    blake3pp::hasher h;
    fake_run(h, content, win, sched);
    CHECK(h.finalize() == blake3pp::hash(content));
  }
  SUBCASE("keyed") {
    blake3pp::hasher h = blake3pp::hasher::keyed(key);
    fake_run(h, content, win, sched);
    CHECK(h.finalize() == blake3pp::keyed_hash(key, content));
  }
  SUBCASE("derive_key") {
    blake3pp::hasher h = blake3pp::hasher::derive_key("blake3pp pipeline test");
    fake_run(h, content, win, sched);
    CHECK(h.finalize() ==
          blake3pp::derive_key("blake3pp pipeline test", content));
  }
}

TEST_CASE("an 8 MiB window is absorbed like any other") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 8 * 1024 * 1024;
  const std::size_t len = 2 * win + 65536;
  const auto content = pattern(len, 72);
  blake3pp::hasher h;
  fake_run(h, content, win, sched, 4, {.order = {1, 0}});
  CHECK(h.finalize() == blake3pp::hash(content));
}

TEST_CASE("prior content decides which path a file takes") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 4 * win + 99;
  const auto content = pattern(len, 73);
  const temp_file f(content);

  // A hasher already on a window boundary keeps the pipeline; one that
  // is not falls back to the sequential loop. Both must agree with a
  // hasher fed the same bytes by hand.
  for (const std::size_t prior : {win, win + 1}) {
    CAPTURE(prior);
    const auto head = pattern(prior, 74);
    blake3pp::hasher h;
    h.update(head);
    blake3pp::update_file(h, f.path, sched, {.window_bytes = win});

    blake3pp::hasher ref;
    ref.update(head);
    ref.update(content);
    CHECK(h.finalize() == ref.finalize());
  }
}

TEST_CASE("a read error at one window surfaces once") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 75);

  for (const int k : {0, 3, 7}) {
    CAPTURE(k);
    blake3pp::hasher h;
    int thrown = 0;
    std::error_code seen;
    try {
      fake_run(h, content, win, sched, 8, {.fail_at_window = k});
    } catch (const std::system_error& e) {
      ++thrown;
      seen = e.code();
    }
    CHECK(thrown == 1);
    CHECK(seen == std::make_error_code(std::errc::io_error));

    // The same failure with tracing on: a traced run is a separate
    // instantiation of the whole scope, so its error path is separate
    // code and would otherwise never run.
    std::vector<blake3pp::window_record> records(16);
    blake3pp::trace_buffer trace(records);
    blake3pp::hasher traced;
    int traced_thrown = 0;
    try {
      fake_driver drv(content, win, {.fail_at_window = k});
      fake_driver::file f(drv);
      blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
          traced, drv, f, sched,
          {.window_bytes = win, .queue_depth = 8, .trace = &trace});
    } catch (const std::system_error&) {
      ++traced_thrown;
    }
    CHECK(traced_thrown == 1);
    // The hasher is left usable, which is the contract update_file
    // already has for a failed read.
    h.update(std::span<const std::byte>(content).first(64));
    CHECK(h.finalize() != blake3pp::digest{});
  }
}

// An exception out of poll() is the one way run() unwinds with windows
// still in flight, and the scope has to wait them out before its
// operation cells go, or a pool thread publishes into freed memory. Two
// shapes, both for the sanitizer lanes: a poll that fails once and then
// serves the rest, and one that never works again, where the window on
// the pool has to come back with no help from the driver and the reads
// the driver still holds are left to it.
TEST_CASE("an exception from poll waits for the pool before unwinding") {
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 91);
  for (const bool keep : {false, true}) {
    CAPTURE(keep);
    b3test::one_thread_pool pool(std::chrono::milliseconds(20));
    fake_driver drv(content, win, {.throw_at_poll = 1, .keep_throwing = keep});
    fake_driver::file f(drv);
    blake3pp::hasher h;
    int thrown = 0;
    try {
      blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
          h, drv, f, pool.get_scheduler(),
          {.window_bytes = win, .queue_depth = 4});
    } catch (const std::runtime_error& e) {
      ++thrown;
      CHECK(std::string_view(e.what()) == "poll failed");
    }
    CHECK(thrown == 1);
    // Whichever way the scope left, the driver holds no read: either the
    // polls served them all, or the give-up branch drained them.
    CHECK(drv.in_flight() == 0);
  }
}

// A submit that throws something other than a system_error reports that
// exception, not a substitute error code: the read sender carries both
// error channels and the scope keeps the first of either.
TEST_CASE("a read the driver could not queue reports what it threw") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 93);
  for (const int k : {0, 2, 7}) {
    CAPTURE(k);
    blake3pp::hasher h;
    int thrown = 0;
    try {
      fake_run(h, content, win, sched, 4, {.bad_alloc_at_window = k});
    } catch (const std::bad_alloc&) {
      ++thrown;
    }
    CHECK(thrown == 1);
  }
}

TEST_CASE("the pipeline runs on an inline scheduler and on one thread") {
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 5 * win + 321;
  const auto content = pattern(len, 76);
  const auto expected = blake3pp::hash(content);

  SUBCASE("inline") {
    // Every part runs inside the driver's own poll; nothing may wait on
    // another thread for it.
    blake3pp::hasher h;
    fake_run(h, content, win, b3test::inline_scheduler{});
    CHECK(h.finalize() == expected);
  }
  SUBCASE("one thread") {
    b3test::one_thread_pool pool;
    blake3pp::hasher h;
    fake_run(h, content, win, pool.get_scheduler());
    CHECK(h.finalize() == expected);
  }
}

TEST_CASE("a driver with no device under it hashes the pattern it serves") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  // The bench measures the pipeline this way: the same window repeated,
  // so the digest is a property of the pipeline and not of any file.
  std::vector<std::byte> content(16 * win);
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<std::byte>(i % 251);
  }
  blake3pp::hasher h;
  fake_run(h, content, win, sched, 8, {.order = {4, 9, 1}});
  CHECK(h.finalize() == blake3pp::hash(content));
}

TEST_CASE("the driver records where its own thread went") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 12 * win + 500;
  const auto content = pattern(len, 81);
  const temp_file f(content);

  std::vector<blake3pp::window_record> records(32);
  blake3pp::trace_buffer trace(records);
  blake3pp::hasher h;
  blake3pp::update_file(h, f.path, sched,
                        {.window_bytes = win, .trace = &trace});
  CHECK(h.finalize() == blake3pp::hash(content));

  const auto& d = trace.driver();
  // One round of the loop per poll at least, one completion off the run
  // queue per window that went to the pool, and time somewhere.
  CHECK(d.iterations > 0);
  CHECK(d.polls > 0);
  CHECK(d.queue_runs >= 12);
  CHECK(d.busy_ns + d.parked_ns > 0);
  CHECK(d.blocking_polls <= d.polls);
  // A wake is a blocking poll released with no completion of its own,
  // so there is at most one per blocking poll, and there need not be
  // any: a driver that finds every window already back on its run
  // queue never sleeps for the pool.
  CHECK(d.wakes <= d.blocking_polls);
  // The default reducer capacity is far above what a twelve-window file
  // decomposes into.
  CHECK(d.admission_stalls == 0);
  MESSAGE("driver busy " << d.busy_ns << " ns, parked " << d.parked_ns
                         << " ns over " << d.iterations << " iterations, "
                         << d.polls << " polls (" << d.blocking_polls
                         << " blocking)");
}

TEST_CASE("the sequential window loop leaves the driver record empty") {
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 3 * win;
  const auto content = pattern(len, 82);
  const temp_file f(content);

  std::vector<blake3pp::window_record> records(8);
  blake3pp::trace_buffer trace(records);
  blake3pp::hasher h;
  // No scheduler: io.hpp's own update_file, which has no driver thread
  // to account for.
  blake3pp::update_file(h, f.path, {.window_bytes = win, .trace = &trace});
  CHECK(h.finalize() == blake3pp::hash(content));
  CHECK(trace.driver().iterations == 0);
  CHECK(trace.driver().busy_ns == 0);
}

// The error path of the real driver, without a fault injector: a file
// is opened through io_driver, which takes its size then, and truncated
// on disk before the pipeline reads it. The windows past the new end
// come back as an unexpected EOF, which the backends report as EIO, and
// that is the one error a device actually produces here.
TEST_CASE("a file truncated under the pipeline fails once with EIO") {
#if defined(_WIN32)
  // The backend opens files FILE_SHARE_READ (src/io/iocp_backend.hpp),
  // so no other handle can get the write access a truncation needs and
  // fs::resize_file fails with a sharing violation. FILE_SHARE_DELETE
  // would not change that -- it permits delete and rename, not a
  // shortening write -- and FILE_SHARE_WRITE, which would, is the wrong
  // trade for a hasher: it would let anything rewrite a file underneath
  // a digest in progress. So the shape this case provokes cannot arise
  // on Windows, and the case says so rather than pretending to cover
  // it. The same error path is covered on every platform through the
  // fake driver; this one covers the real backend's.
  MESSAGE("skipped: the backend's share mode makes an open file untruncatable");
#else
  using blake3pp::detail::io_driver;
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 91);

  const auto run_truncated = [&](blake3pp::trace_buffer* trace) {
    const temp_file f(content);
    io_driver drv({/*async=*/true, /*offload_submit=*/true}, 8);
    // The size the pipeline will plan its windows from is taken here.
    io_driver::file file(drv, f.path, /*direct_io=*/false);
    REQUIRE(file.size() == len);
    // Everything from the second window on is now gone.
    std::error_code fs_ec;
    fs::resize_file(f.path, win, fs_ec);
    REQUIRE(!fs_ec);

    blake3pp::hasher h;
    int thrown = 0;
    std::error_code seen;
    try {
      blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
          h, drv, file, sched,
          {.window_bytes = win, .queue_depth = 8, .trace = trace});
    } catch (const std::system_error& e) {
      ++thrown;
      seen = e.code();
    }
    CHECK(thrown == 1);
    CHECK(seen == std::error_code(EIO, std::generic_category()));
  };

  SUBCASE("untraced") { run_truncated(nullptr); }
  SUBCASE("traced") {
    std::vector<blake3pp::window_record> records(16);
    blake3pp::trace_buffer trace(records);
    run_truncated(&trace);
    // The windows that were claimed before the failure are still
    // consistent records; the run just stops early.
    CHECK(trace.windows().size() <= 8);
  }
#endif
}

// The last window is where the tree stops and the hasher takes over, so
// every length that moves that boundary is its own case: the part that
// holds the final chunk stays with the hasher, and everything before it
// has to reach the tree unchanged.
TEST_CASE("every last-window length hashes like the sequential path") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  // The part size this window count produces, so the lengths either side
  // of a part boundary can be named.
  constexpr std::size_t part = 16 * blake3pp::chunk_size;
  const std::size_t tails[] = {
      1,             // a single byte
      1024,          // one chunk
      part - 1,      // just under a part
      part,          // exactly a part
      part + 1,      // just over
      2 * part,      // two parts: the smallest fan-out
      win - 1,       // a window all but a byte
      win,           // an exact window multiple
  };
  for (const std::size_t leading : {std::size_t{0}, std::size_t{3}}) {
    for (const std::size_t tail : tails) {
      const std::size_t len = leading * win + tail;
      CAPTURE(leading);
      CAPTURE(tail);
      const auto content = pattern(len, static_cast<std::uint32_t>(tail));
      const auto expected = blake3pp::hash(content);

      blake3pp::hasher fake;
      fake_run(fake, content, win, sched, 4, {.order = {2, 0, 3}});
      CHECK(fake.finalize() == expected);

      // The same through the real driver, which sizes its windows from
      // the file's own length rather than from a span.
      const temp_file f(content);
      CHECK(pipeline_digest(f.path, win, 4, 0) == expected);
      CHECK(pipeline_digest(f.path, win, 4, 1) == expected);
    }
  }
}

TEST_CASE("the last window fans out in every hasher mode") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  // Three windows and a last one big enough to have parts of its own.
  const std::size_t len = 3 * win + 48 * 1024;
  const auto content = pattern(len, 101);
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i * 3 + 5);
  }

  SUBCASE("plain") {
    blake3pp::hasher h;
    fake_run(h, content, win, sched);
    CHECK(h.finalize() == blake3pp::hash(content));
  }
  SUBCASE("keyed") {
    blake3pp::hasher h = blake3pp::hasher::keyed(key);
    fake_run(h, content, win, sched);
    CHECK(h.finalize() == blake3pp::keyed_hash(key, content));
  }
  SUBCASE("derive_key") {
    blake3pp::hasher h = blake3pp::hasher::derive_key("blake3pp last window");
    fake_run(h, content, win, sched);
    CHECK(h.finalize() == blake3pp::derive_key("blake3pp last window", content));
  }
}

TEST_CASE("an 8 MiB last window fans out too") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 8 * 1024 * 1024;
  // A last window of six sevenths of a window: plenty of parts before
  // the one holding the final chunk.
  const std::size_t len = win + (win * 6) / 7;
  const auto content = pattern(len, 102);
  blake3pp::hasher h;
  fake_run(h, content, win, sched, 4, {.order = {1, 0}});
  CHECK(h.finalize() == blake3pp::hash(content));
}

TEST_CASE("a last window with many parts never refuses an insert") {
  using blake3pp::default_stack_budget;
  using scope_type =
      blake3pp::detail::window_scope<false, default_stack_budget, fake_driver,
                                     blake3pp::parallel_scheduler_t>;
  constexpr std::size_t win = 64 * 1024;
  // Four windows, the last one all but a byte short of full: its parts
  // decompose into several nodes at once, which is what the reservation
  // has to cover.
  const std::size_t len = 3 * win + win - 1;
  const auto content = pattern(len, 103);
  fake_driver drv(content, win, {.order = {3, 1, 0, 2}});
  fake_driver::file f(drv);
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  // Just above what this file needs: three nodes for a run of three
  // windows, plus the last window's own reservation.
  pooled_scope<scope_type, fake_driver, decltype(sched)> ps(h, drv, f, sched,
                   {.window_bytes = win,
                    .queue_depth = 4,
                    .trace = nullptr,
                    .in_flight_cap = 0,
                    .reducer_capacity = 12});
  ps.scope.run();
  CHECK(h.finalize() == blake3pp::hash(content));
}

// A capacity below what the file needs is a configuration the pipeline
// cannot run. Unreported, it is a driver asleep for ever, so the scope
// throws, in every build, since a release lane has no asserts to fire.
TEST_CASE("a reducer capacity below the floor is reported, not a hang") {
  using blake3pp::default_stack_budget;
  using scope_type =
      blake3pp::detail::window_scope<false, default_stack_budget, fake_driver,
                                     blake3pp::parallel_scheduler_t>;
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 111);

  fake_driver drv(content, win, {});
  fake_driver::file f(drv);
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  // Two nodes cannot hold the three a run of seven windows leaves, let
  // alone the last window's reservation on top.
  pooled_scope<scope_type, fake_driver, decltype(sched)> ps(h, drv, f, sched,
                   {.window_bytes = win,
                    .queue_depth = 8,
                    .trace = nullptr,
                    .in_flight_cap = 0,
                    .reducer_capacity = 2});
  CHECK_THROWS_AS(ps.scope.run(), std::logic_error);
}

// The window count is arithmetic over four inputs, so it is checked as
// arithmetic: the budget buys windows beyond the reads, the reads are
// the floor, max_queue_depth the ceiling, and the file the last word.
TEST_CASE("plan_pipeline sizes the windows from the budget, the reads and the file") {
  using blake3pp::detail::default_window_budget_bytes;
  using blake3pp::detail::max_queue_depth;
  using blake3pp::detail::plan_pipeline;
  constexpr std::size_t mib = std::size_t{1} << 20;
  constexpr std::uint64_t large = std::uint64_t{64} << 30;
  static_assert(default_window_budget_bytes == 128 * mib);

  // A large file at 8 MiB x qd 4: the budget buys 16 windows, the
  // device still sees 4 reads.
  {
    const auto p = plan_pipeline(0, 8 * mib, 4, false, large);
    CHECK(p.reads_in_flight == 4);
    CHECK(p.windows == 16);
  }
  // Large windows: the budget buys two, which is below the reads, so
  // the reads are the floor and memory is what it was.
  {
    const auto p = plan_pipeline(0, 64 * mib, 4, false, large);
    CHECK(p.reads_in_flight == 4);
    CHECK(p.windows == 4);
    CHECK(p.windows * p.window_bytes == 256 * mib);
  }
  // A deeper queue lifts the floor with it.
  CHECK(plan_pipeline(0, 64 * mib, 8, false, large).windows == 8);
  // Small windows: the budget would buy 2048; the ceiling holds.
  {
    const auto p = plan_pipeline(0, 64 * 1024, 4, false, large);
    CHECK(p.windows == max_queue_depth);
    CHECK(p.reads_in_flight == 4);
  }
  // The ceiling also bounds a queue depth asked above it.
  CHECK(plan_pipeline(0, 8 * mib, 64, false, large).reads_in_flight ==
        max_queue_depth);
  CHECK(plan_pipeline(0, 8 * mib, 64, false, large).windows == max_queue_depth);
  // A small file gets exactly its windows, edges included.
  CHECK(plan_pipeline(0, 64 * 1024, 4, false, 3 * 64 * 1024).windows == 3);
  CHECK(plan_pipeline(0, 64 * 1024, 4, false, 3 * 64 * 1024 + 1).windows == 4);
  CHECK(plan_pipeline(0, 64 * 1024, 4, false, 1).windows == 1);
  // A short first window counts as one, then the rest on the grid.
  {
    const auto p = plan_pipeline(1000, 64 * 1024, 4, false, 2 * 64 * 1024);
    CHECK(p.head_bytes == 64 * 1024 - 1000);
    CHECK(p.windows == 3);
  }
  // An unknown size leaves the count to the budget; an empty file has
  // no windows at all.
  CHECK(plan_pipeline(0, 8 * mib, 4, false, 0).windows == 16);
}

// The two bounds, apart: with two reads allowed and eight windows, the
// device never sees a third read while the pool, held slow by a
// one-thread scheduler that sleeps per node, fills every window object.
// The cap on windows in flight still wins over both.
TEST_CASE("reads in flight stay under the plan while windows fill the pool") {
  using blake3pp::detail::pipeline_plan;
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 16 * win;
  const auto content = pattern(len, 97);
  const pipeline_plan plan{win, 2, 8, 0, false};

  for (const unsigned cap : {0u, 1u}) {
    CAPTURE(cap);
    b3test::one_thread_pool pool(std::chrono::milliseconds(5));
    fake_driver drv(content, win);
    fake_driver::file f(drv);
    std::vector<blake3pp::window_record> records(20);
    blake3pp::trace_buffer trace(records);
    blake3pp::hasher h;
    blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
        h, drv, f, pool.get_scheduler(), plan,
        {.window_bytes = win,
         .queue_depth = 2,
         .trace = &trace,
         .in_flight_cap = cap});
    CHECK(h.finalize() == blake3pp::hash(content));
    CHECK(drv.pool_bytes() == 8 * win);
    const auto& d = trace.driver();
    CHECK(d.max_reads_in_flight <= 2);
    if (cap == 0) {
      CHECK(d.max_windows_in_flight == 8);
    } else {
      CHECK(d.max_windows_in_flight == 1);
      CHECK(d.max_reads_in_flight == 1);
    }
    MESSAGE("cap " << cap << ": max reads " << d.max_reads_in_flight
                   << ", max windows " << d.max_windows_in_flight);
  }
}

// A file of three windows gets three window objects, whatever the
// budget would buy.
TEST_CASE("a three-window file allocates exactly three windows") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const auto content = pattern(3 * win, 98);
  fake_driver drv(content, win);
  fake_driver::file f(drv);
  blake3pp::hasher h;
  blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
      h, drv, f, sched, {.window_bytes = win, .queue_depth = 4});
  CHECK(h.finalize() == blake3pp::hash(content));
  CHECK(drv.pool_bytes() == 3 * win);
}

// A hasher part-way through a message is brought back onto the window
// grid by one short first window. Every prior count that moves that
// boundary is its own case, and the open mode moves with it: an
// unaligned grid cannot use direct I/O at all, because every window
// after the short one starts unaligned too.
TEST_CASE("prior content sets the window grid and the open mode") {
  using blake3pp::detail::direct_io_fits;
  using blake3pp::detail::first_window_bytes;
  using blake3pp::detail::io_driver;
  using blake3pp::detail::plan_pipeline;
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::size_t len = 3 * win + 1234;
  const auto content = pattern(len, 121);
  const temp_file f(content);

  const std::size_t priors[] = {
      0,             // a fresh hasher: no short window at all
      win,           // already on a window boundary
      8 * 1024,      // a chunk multiple, and a multiple of 4096
      9 * 1024,      // a chunk multiple that is not a multiple of 4096
      100,           // not even a chunk
      win - 1,       // one byte short of the grid
  };
  for (const std::size_t prior : priors) {
    CAPTURE(prior);
    const auto head_content = pattern(prior, 122);
    const std::size_t head = first_window_bytes(prior, win);
    const bool direct_ok = direct_io_fits(head);
    CHECK(direct_ok == (head % io_driver::direct_alignment == 0));
    // The plan update_file makes carries the same two answers.
    const auto plan = plan_pipeline(prior, win, 4, true, len);
    CHECK(plan.window_bytes == win);
    CHECK(plan.head_bytes == head);
    CHECK(plan.direct_io == direct_ok);
    CHECK(plan_pipeline(prior, win, 4, false, len).direct_io == false);

    blake3pp::hasher h;
    h.update(head_content);
    blake3pp::update_file(h, f.path, sched, {.window_bytes = win});

    blake3pp::hasher ref;
    ref.update(head_content);
    ref.update(content);
    CHECK(h.finalize() == ref.finalize());

    // What update_file decides about the open mode, checked where the
    // decision shows: a grid the alignment cannot serve must never
    // produce a direct-I/O file, whatever the filesystem would allow.
    io_driver drv({}, 4);
    io_driver::file file(drv, f.path, /*direct_io=*/direct_ok);
    CAPTURE(file.name());
    if (!direct_ok) {
      CHECK(file.name().find("direct") == std::string_view::npos);
      CHECK(file.name().find("nocache") == std::string_view::npos);
    }
  }
}

TEST_CASE("several files hash as one message, every one on the pool") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  // Sizes that are not window multiples, so every file after the first
  // starts part-way through the grid.
  const std::size_t sizes[] = {3 * win + 777, 2 * win + 4095, 5 * win + 1};
  std::vector<std::byte> joined;
  std::vector<std::unique_ptr<temp_file>> files;
  for (const std::size_t n : sizes) {
    const auto content = pattern(n, static_cast<std::uint32_t>(n));
    joined.insert(joined.end(), content.begin(), content.end());
    files.push_back(std::make_unique<temp_file>(content));
  }

  std::vector<blake3pp::window_record> records(128);
  blake3pp::trace_buffer trace(records);
  blake3pp::hasher h;
  for (const auto& file : files) {
    blake3pp::update_file(h, file->path, sched,
                          {.window_bytes = win, .trace = &trace});
  }
  CHECK(h.finalize() == blake3pp::hash(joined));
  CHECK(h.count() == joined.size());

  // Every file's windows reached the pool, including the files that
  // began off the grid: that is what the short first window buys.
  std::size_t fanned = 0;
  std::size_t short_first = 0;
  for (const auto& r : trace.windows()) {
    if ((r.flags & blake3pp::window_record::flag_parallel) != 0) {
      ++fanned;
    }
    if ((r.flags & blake3pp::window_record::flag_short_first) != 0) {
      ++short_first;
    }
  }
  CHECK(fanned >= 3 * std::size(sizes));
  // Two of the three files start off the grid.
  CHECK(short_first == 2);
}

}  // TEST_SUITE
