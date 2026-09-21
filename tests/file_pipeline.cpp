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

}  // TEST_SUITE
