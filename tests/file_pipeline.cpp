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
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <system_error>
#include <utility>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <vector>

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
    queued_.push_back(read{&op, off, buf, 0});
  }

  void flush() noexcept {}

  std::size_t poll(bool block) {
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
    const unsigned index = static_cast<unsigned>(r.off / window_bytes_);
    if (script_.fail_at_window >= 0 &&
        index == static_cast<unsigned>(script_.fail_at_window)) {
      blake3pp::detail::io_read_op* const op = r.op;
      queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
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

  [[nodiscard]] std::size_t in_flight() const noexcept {
    return queued_.size();
  }

  [[nodiscard]] std::span<std::byte> allocate(std::size_t bytes) {
    pool_.resize(bytes);
    return {pool_.data(), pool_.size()};
  }

 private:
  struct read {
    blake3pp::detail::io_read_op* op;
    std::uint64_t off;
    std::span<std::byte> buf;
    std::size_t filled;
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
  const auto pool = drv.allocate(len);
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
  const auto pool = drv.allocate(len);

  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  auto op = ex::connect(io.read(file, 0, pool), chain_receiver{&r, &st});
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
  const auto pool = drv.allocate(len);

  // Past the end of the file: the backend reports the unexpected EOF as
  // an error, and the chain sees it as std::error_code, not a value.
  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  auto op = ex::connect(io.read(file, len, pool), chain_receiver{&r, &st});
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
  const auto pool = drv.allocate(len);

  // Two chains, so the loop runs until both are in: the read is what
  // makes blocking legitimate, and the other thread's schedule is what
  // has to get through to a driver already asleep.
  blake3pp::detail::loop_status st{false, 2};
  chain_result read_done;
  auto read_op =
      ex::connect(io.read(file, 0, pool), chain_receiver{&read_done, &st});
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
  const auto pool = drv.allocate(len);

  // The shape of the real window chain, with a stand-in for the compress
  // stage: read on the driver, work on the pool, back on the driver.
  // Phase 4 replaces only the middle line, so this is the case that says
  // whether the shape itself holds on this provider.
  auto sched = blake3pp::get_parallel_scheduler();
  const auto here = std::this_thread::get_id();
  std::atomic<bool> summed_elsewhere{false};
  std::thread::id returned_on{};

  chain_result r;
  blake3pp::detail::loop_status st{false, 1};
  auto op = ex::connect(
      ex::then(
          ex::continues_on(
              ex::let_value(io.read(file, 0, pool),
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

TEST_CASE("sibling windows the reducer cannot hold still finish") {
  // The liveness case admission control exists for. With a retry list
  // instead, two windows refused one after the other could never merge:
  // the second looks for its sibling inside the reducer, and the first is
  // held outside it. Every window ended up held, nothing was in flight
  // and the run never finished.
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
  // Four nodes is the smallest capacity this file may legally run with:
  // a contiguous run of k of its eight windows holds popcount(k) nodes,
  // three at k = 7, and the rule needs one more than that. At two the
  // scope stalls by construction, which is what the assert in the start
  // hook says.
  scope_type scope(h, drv, f, sched,
                   {.window_bytes = win,
                    .queue_depth = 8,
                    .trace = nullptr,
                    .in_flight_cap = 0,
                    .reducer_capacity = 4});
  run_scope(scope);
  CHECK(h.finalize() == blake3pp::hash(content));
}

TEST_CASE("admission throttles starts instead of refusing an insert") {
  using blake3pp::default_stack_budget;
  using scope_type =
      blake3pp::detail::window_scope<false, default_stack_budget, fake_driver,
                                     blake3pp::parallel_scheduler_t>;
  constexpr std::size_t win = 64 * 1024;
  // Eight windows: a contiguous run of k of them holds popcount(k)
  // nodes, three at most, so four is just above what this file needs and
  // far below the eight windows that would otherwise start at once.
  const std::size_t len = 8 * win;
  const auto content = pattern(len, 62);

  fake_driver drv(content, win, {.order = {3, 1, 0, 2}});
  fake_driver::file f(drv);
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  scope_type scope(h, drv, f, sched,
                   {.window_bytes = win,
                    .queue_depth = 8,
                    .trace = nullptr,
                    .in_flight_cap = 0,
                    .reducer_capacity = 4});
  run_scope(scope);
  CHECK(h.finalize() == blake3pp::hash(content));
  // The whole point: the scope held windows back rather than letting the
  // reducer refuse one.
  CHECK(scope.admission_stalls() > 0);
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
  scope_type scope(h, drv, f, sched,
                   {.window_bytes = win, .queue_depth = 8});
  run_scope(scope);
  CHECK(h.finalize() == blake3pp::hash(content));
  CHECK(scope.admission_stalls() == 0);
}

// A scheduler that runs the work where it was started: the case that
// must not deadlock, since the compress stage then runs inside the
// driver's own poll.
struct inline_scheduler;

template <class Receiver>
struct inline_op {
  using operation_state_concept = compat::operation_state_tag;
  inline_op(Receiver r) : rcvr(std::move(r)) {}
  Receiver rcvr;
  void start() & noexcept { ex::set_value(std::move(rcvr)); }
};

struct inline_env {
  [[nodiscard]] inline_scheduler query(
      ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept;
};

struct inline_sender {
  using sender_concept = compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t());
  template <class Receiver>
  inline_op<Receiver> connect(Receiver r) const {
    return {std::move(r)};
  }
  [[nodiscard]] inline_env get_env() const noexcept { return {}; }
};

struct inline_scheduler : compat::weakly_parallel_scheduler {
  using scheduler_concept = compat::scheduler_tag;
  [[nodiscard]] inline_sender schedule() const noexcept { return {}; }
  bool operator==(const inline_scheduler&) const noexcept = default;
};

inline inline_scheduler inline_env::query(
    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
  return {};
}

static_assert(ex::scheduler<inline_scheduler>);

// One worker thread and a queue: the other end of the range, where every
// part of every window is serialized behind a single agent.
class one_thread_pool {
 public:
  one_thread_pool() : worker_([this] { run(); }) {}
  ~one_thread_pool() {
    {
      const std::lock_guard lock(m_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  one_thread_pool(const one_thread_pool&) = delete;
  one_thread_pool& operator=(const one_thread_pool&) = delete;

  void post(run_node* n) {
    {
      const std::lock_guard lock(m_);
      queue_.push_back(n);
    }
    cv_.notify_one();
  }

  struct scheduler;

  template <class Receiver>
  struct op : run_node {
    using operation_state_concept = compat::operation_state_tag;
    op(Receiver r, one_thread_pool* p) : rcvr(std::move(r)), pool(p) {
      run = [](run_node* self) noexcept {
        ex::set_value(std::move(static_cast<op*>(self)->rcvr));
      };
    }
    Receiver rcvr;
    one_thread_pool* pool;
    void start() & noexcept { pool->post(this); }
  };

  struct env {
    one_thread_pool* pool;
    [[nodiscard]] scheduler query(
        ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept;
  };

  struct sender {
    using sender_concept = compat::sender_tag;
    BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t());
    one_thread_pool* pool;
    template <class Receiver>
    op<Receiver> connect(Receiver r) const {
      return {std::move(r), pool};
    }
    [[nodiscard]] env get_env() const noexcept { return {pool}; }
  };

  struct scheduler : compat::weakly_parallel_scheduler {
    using scheduler_concept = compat::scheduler_tag;
    one_thread_pool* pool = nullptr;
    [[nodiscard]] sender schedule() const noexcept { return {pool}; }
    bool operator==(const scheduler&) const noexcept = default;
  };

  [[nodiscard]] scheduler get_scheduler() noexcept { return {{}, this}; }

 private:
  void run() {
    for (;;) {
      run_node* n = nullptr;
      {
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stop_) {
            return;
          }
          continue;
        }
        n = queue_.front();
        queue_.erase(queue_.begin());
      }
      n->run(n);
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::vector<run_node*> queue_;
  bool stop_ = false;
  std::thread worker_;
};

inline one_thread_pool::scheduler one_thread_pool::env::query(
    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
  return {{}, pool};
}

static_assert(ex::scheduler<one_thread_pool::scheduler>);

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
    // The hasher is left usable, which is the contract update_file
    // already has for a failed read.
    h.update(std::span<const std::byte>(content).first(64));
    CHECK(h.finalize() != blake3pp::digest{});
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
    fake_run(h, content, win, inline_scheduler{});
    CHECK(h.finalize() == expected);
  }
  SUBCASE("one thread") {
    one_thread_pool pool;
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

}  // TEST_SUITE
