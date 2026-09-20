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

}  // TEST_SUITE
