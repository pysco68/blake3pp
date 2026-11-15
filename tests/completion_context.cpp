// The shared completion context, driven by a scripted engine: the rules
// every completion backend gets from src/io/completion_context.hpp,
// tested on every platform and under the sanitizers without a kernel
// queue. Each backend's own engine is tested in io_context.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "io/completion_context.hpp"
#include "io/transfer.hpp"

// Named rather than anonymous: the engine's constants are its contract
// with the context, not values this file reads.
namespace fake_completion {

namespace io_impl = blake3pp::detail::io_impl;

// The byte the fake file holds at an offset.
[[nodiscard]] std::byte byte_at(std::uint64_t off) noexcept {
  return static_cast<std::byte>(static_cast<unsigned char>(off * 131 + 7));
}

void fill(std::span<std::byte> to, std::uint64_t off) noexcept {
  for (std::size_t i = 0; i < to.size(); ++i) {
    to[i] = byte_at(off + i);
  }
}

// A kernel queue that does what the test script says: completes the
// pieces it was given in order, as short as `piece`, refuses what it is
// told to refuse, and answers a wait that could never end with a failure
// rather than a hang.
class fake_engine {
 public:
  struct transfer_part : io_impl::span_transfer {
    template <class Part>
    void arm_read(Part&, std::uint64_t at, std::span<std::byte> to) noexcept {
      arm_span_read(at, to);
    }
    template <class Part>
    void arm_write(Part&, std::uint64_t at,
                   std::span<const std::byte> from) noexcept {
      arm_span_write(at, from);
    }
  };

  class file_part {
   public:
    file_part(const fake_engine&, const std::filesystem::path&,
              const io_impl::file_open_options&) noexcept {}
    [[nodiscard]] std::uint64_t size() const noexcept {
      return std::uint64_t{1} << 30;
    }
    [[nodiscard]] std::string_view name() const noexcept { return "fake"; }
    void set_size(std::uint64_t) noexcept {}
  };

  struct below {
    static constexpr blake3pp::io_engine rung_engine =
        blake3pp::io_engine::stdio;
  };
  static constexpr blake3pp::io_engine rung_engine =
      blake3pp::io_engine::io_uring;
  using rung_below = below;
  [[nodiscard]] std::string_view mechanism() const noexcept {
    return "the fake engine";
  }

  fake_engine(const io_impl::reader_context_options& opts, unsigned) noexcept
      : engaged_(opts.async) {}

  // The script.
  std::size_t piece = std::numeric_limits<std::size_t>::max();
  bool takes_rest = true;         // false: a remainder goes to the pump
  bool submits_on_flush = false;  // pieces wait for flush(), as on a ring
  int refuse_flushes = 0;         // flushes the kernel refuses outright
  bool fail_submit = false;       // a flush latches a failure, submits nothing
  std::error_code latched{};      // what take_failure() reports, once
  bool fail_wait = false;         // a blocking next() answers failed
  bool wake_first = false;        // a pending wake comes before completions

  // What the engine saw.
  std::size_t placed = 0;
  std::size_t released = 0;
  std::size_t submits = 0;  // flushes that gave the kernel something
  std::size_t blocked_waits = 0;  // blocking looks that had to wait
  std::size_t blocked_unsubmitted = 0;

  // A control entry the kernel owes, a cancel's completion or a wake
  // poll: next() consumes it as progress.
  void post_sentinel() { kernel_.push_back(nullptr); }

  [[nodiscard]] io_impl::rung_verdict engaged_rung() const noexcept {
    return {rung_engine, true};
  }
  [[nodiscard]] bool engaged() const noexcept { return engaged_; }
  [[nodiscard]] unsigned device_depth(bool used, unsigned lanes) const noexcept {
    return used ? 4u : lanes;
  }

  // A lane's transfer, which a test can hold at a gate: in_perform says a
  // lane has taken one, hold_lanes keeps it there.
  static inline std::atomic<int> in_perform{0};
  static inline std::atomic<bool> hold_lanes{false};

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    in_perform.fetch_add(1);
    while (hold_lanes.load()) {
      hold_lanes.wait(true);
    }
    if (!op.is_write) {
      fill(op.read_remaining(), op.position());
    }
    op.filled = op.length();
    return {};
  }

  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    if (!engaged_ || op.remaining() == 0 || (op.filled > 0 && !takes_rest)) {
      return false;
    }
    op.device_begin();
    ++placed;
    queued_.push_back(&op);
    if (!submits_on_flush) {
      submit();
    }
    return true;
  }
  template <class Op>
  void cancel(Op&) noexcept {}

  template <class Op, class Refused>
  void flush(Refused&& refused) noexcept {
    if (queued_.empty()) {
      return;
    }
    if (fail_submit) {
      latched = std::make_error_code(std::errc::io_error);
      return;
    }
    if (refuse_flushes > 0) {
      --refuse_flushes;
      take_back<Op>(refused);
      return;
    }
    submit();
  }
  template <class Op, class Refused>
  void take_back(Refused&& refused) noexcept {
    while (!queued_.empty()) {
      transfer_part* const p = queued_.front();
      queued_.pop_front();
      p->device_end();
      refused(static_cast<Op&>(*p));
    }
  }
  [[nodiscard]] std::error_code take_failure() noexcept {
    return std::exchange(latched, {});
  }

  template <class Op>
  [[nodiscard]] io_impl::engine_event<Op> next(bool block) noexcept {
    using event = io_impl::engine_event<Op>;
    if (wake_first && woken_.exchange(false)) {
      return {event::kind::wake};
    }
    if (!kernel_.empty()) {
      transfer_part* const front = kernel_.front();
      kernel_.pop_front();
      if (front == nullptr) {
        return {event::kind::progress};
      }
      Op& op = static_cast<Op&>(*front);
      if (op.cancelled) {
        return {event::kind::completion, &op,
                std::make_error_code(std::errc::operation_canceled), 0};
      }
      const std::size_t n = std::min(piece, op.remaining());
      if (!op.is_write) {
        fill(op.read_remaining().first(n), op.position());
      }
      return {event::kind::completion, &op, {}, n};
    }
    if (woken_.exchange(false)) {
      return {event::kind::wake};
    }
    if (!block) {
      return {};
    }
    if (fail_wait) {
      return {event::kind::failed, nullptr,
              std::make_error_code(std::errc::io_error)};
    }
    if (!queued_.empty()) {
      // A real kernel would sleep for ever here: what it is waited on
      // for was never given to it.
      ++blocked_unsubmitted;
      return {event::kind::failed, nullptr,
              std::make_error_code(std::errc::resource_deadlock_would_occur)};
    }
    // Nothing in the kernel: only a lane's wake can end the wait.
    ++blocked_waits;
    woken_.wait(false);
    woken_.store(false);
    return {event::kind::wake};
  }

  void wake() noexcept {
    woken_.store(true);
    woken_.notify_one();
  }

  template <class Part>
  void cancel_file(Part&, std::size_t) noexcept {}
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&& mine) noexcept {
    const auto n = std::erase_if(queued_, [&](transfer_part* p) {
      return mine(static_cast<const Op&>(*p));
    });
    return static_cast<std::size_t>(n);
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept { return kernel_.size(); }
  void begin_drain() noexcept {
    queued_.clear();
  }
  void end_drain() noexcept {}
  template <class Op>
  void release(Op&) noexcept {
    ++released;
  }

 private:
  void submit() {
    if (queued_.empty()) {
      return;
    }
    ++submits;
    kernel_.insert(kernel_.end(), queued_.begin(), queued_.end());
    queued_.clear();
  }

  bool engaged_ = true;
  std::deque<transfer_part*> queued_;  // placed, not yet submitted
  std::deque<transfer_part*> kernel_;  // submitted, owed a completion
  std::atomic<bool> woken_{false};
};

using context = io_impl::completion_context<fake_engine>;

struct probe {
  int calls = 0;
  std::error_code ec{};
  static void on_done(io_impl::transfer_base* base,
                      std::error_code e) noexcept {
    auto* const p = static_cast<probe*>(base->owner);
    p->calls++;
    p->ec = e;
  }
};

void arm(context::transfer& op, probe& p) {
  op.done = &probe::on_done;
  op.owner = &p;
}

// Polls until `p` has run, adding up what poll() said it ran.
std::size_t poll_until(context& ctx, const probe& p) {
  std::size_t reported = 0;
  for (int polls = 0; p.calls == 0; ++polls) {
    REQUIRE(polls < 1000);
    reported += ctx.poll(true);
  }
  return reported;
}

[[nodiscard]] bool holds_pattern(std::span<const std::byte> got,
                                 std::uint64_t off) {
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i] != byte_at(off + i)) {
      return false;
    }
  }
  return true;
}

}  // namespace fake_completion

using namespace fake_completion;

TEST_SUITE("completion_context") {

// A short piece's remainder goes back on the queue during the reap, and a
// queue whose wait does not submit would then sleep on an entry it was
// never given. io_uring is such a queue; the completion port is not.
TEST_CASE("a remainder placed during a reap is submitted before poll blocks") {
  context ctx({}, 4);
  ctx.engine().piece = 4096;
  ctx.engine().submits_on_flush = true;
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(4 * 4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 1000, std::span(got), op);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(!p.ec);
  CHECK(holds_pattern(got, 1000));
  CHECK(ctx.engine().placed == 4);
  CHECK(ctx.engine().blocked_unsubmitted == 0);
  CHECK(ctx.served_synchronously() == 0);
  CHECK(ctx.in_flight() == 0);
}

// The rest of a transfer the engine will not take again finishes on a
// lane; poll() reports the one callback, not the hand-over.
TEST_CASE("poll() counts callbacks, not a remainder moved to the pump") {
  context ctx({}, 4);
  ctx.engine().piece = 4096;
  ctx.engine().takes_rest = false;
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(3 * 4096 + 100);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(!p.ec);
  CHECK(holds_pattern(got, 0));
  CHECK(ctx.served_synchronously() == 1);
}

TEST_CASE("a submission the kernel refuses is served by the lanes") {
  context ctx({}, 4);
  ctx.engine().submits_on_flush = true;
  ctx.engine().refuse_flushes = 1;
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(8192);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(!p.ec);
  CHECK(holds_pattern(got, 0));
  CHECK(ctx.served_synchronously() == 1);
}

TEST_CASE("an engine that was never had serves everything on the lanes") {
  context ctx({.async = false}, 4);
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(holds_pattern(got, 0));
  CHECK(ctx.engine().placed == 0);
  CHECK(ctx.served_synchronously() == 1);
}

TEST_CASE("a latched engine failure is thrown once, from poll") {
  context ctx({}, 4);
  ctx.engine().latched = std::make_error_code(std::errc::io_error);
  CHECK_THROWS_AS(ctx.poll(false), std::system_error);
  CHECK(ctx.poll(false) == 0);
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(!p.ec);
}

TEST_CASE("a failed wait reaches the caller, naming the engine") {
  context ctx({}, 4);
  ctx.engine().fail_wait = true;
  try {
    (void)ctx.poll(true);
    FAIL("poll() returned from a wait that failed");
  } catch (const std::system_error& e) {
    CHECK(e.code() == std::errc::io_error);
    CHECK(std::string(e.what()).find("the fake engine") != std::string::npos);
  }
}

TEST_CASE("a cancelled transfer on the engine reports operation_canceled once") {
  context ctx({}, 4);
  ctx.engine().submits_on_flush = true;
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.cancel(op);
  ctx.cancel(op);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(p.ec == std::errc::operation_canceled);
  CHECK(ctx.poll(false) == 0);
  CHECK(p.calls == 1);
}

// A file closes with one transfer the kernel holds, one it was never
// given, and one on the pump. None of them is reported, and a sibling's
// transfer the kernel holds behind them stays owed.
TEST_CASE("a closed file takes its unreported transfers with it") {
  context ctx({}, 4);
  ctx.engine().submits_on_flush = true;
  context::file sibling(ctx, "fake", {});
  std::vector<std::byte> sibling_got(4096);
  probe sp;
  context::transfer sop{};
  arm(sop, sp);

  std::vector<std::byte> a(4096);
  std::vector<std::byte> b(4096);
  probe p;
  context::transfer in_kernel{};
  context::transfer unsubmitted{};
  context::transfer on_pump{};
  arm(in_kernel, p);
  arm(unsubmitted, p);
  arm(on_pump, p);
  {
    context::file file(ctx, "fake", {});
    ctx.submit_read(file, 0, std::span(a), in_kernel);
    ctx.submit_read(sibling, 0, std::span(sibling_got), sop);
    ctx.flush();
    ctx.submit_read(file, 4096, std::span(b), unsubmitted);
    // Zero bytes is a transfer no kernel queue takes.
    ctx.submit_read(file, 0, std::span<std::byte>{}, on_pump);
    CHECK(ctx.in_flight() == 4);
  }
  CHECK(ctx.in_flight() == 1);
  CHECK(p.calls == 0);
  CHECK(sp.calls == 0);
  CHECK(poll_until(ctx, sp) == 1);
  CHECK(holds_pattern(sibling_got, 0));
  CHECK(ctx.poll(false) == 0);
  CHECK(p.calls == 0);
  CHECK(ctx.in_flight() == 0);
}

// The queue is shared, so a sibling's completion can surface while a
// closing file waits its own out. It is reported then, from the close:
// dropped, it would strand an operation that can never complete.
TEST_CASE("a sibling's completion that surfaces while a file closes is reported") {
  context ctx({}, 4);
  context::file sibling(ctx, "fake", {});
  std::vector<std::byte> sibling_got(4096);
  probe sp;
  context::transfer sop{};
  arm(sop, sp);
  ctx.submit_read(sibling, 0, std::span(sibling_got), sop);
  std::vector<std::byte> a(4096);
  probe p;
  context::transfer mine{};
  arm(mine, p);
  {
    context::file file(ctx, "fake", {});
    ctx.submit_read(file, 0, std::span(a), mine);
    ctx.flush();
  }
  CHECK(sp.calls == 1);
  CHECK(!sp.ec);
  CHECK(holds_pattern(sibling_got, 0));
  CHECK(p.calls == 0);
  CHECK(ctx.in_flight() == 0);
}

// Closing a file that owes nothing does not wait for the lanes, which may
// be busy with a sibling's transfer for as long as the device takes. The
// sibling's lane is held at a gate; a watchdog would open it after ten
// seconds, and the close must not have needed it to.
TEST_CASE("closing a file that owes nothing does not wait for a busy lane") {
  context ctx({.async = false}, 4);
  context::file busy(ctx, "fake", {});
  std::vector<std::byte> got(4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  fake_engine::in_perform.store(0);
  fake_engine::hold_lanes.store(true);
  std::atomic<bool> watchdog_fired{false};
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load() && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load()) {
      watchdog_fired.store(true);
    }
    fake_engine::hold_lanes.store(false);
    fake_engine::hold_lanes.notify_all();
  });
  ctx.submit_read(busy, 0, std::span(got), op);
  ctx.flush();
  while (fake_engine::in_perform.load() == 0 && !watchdog_fired.load()) {
    std::this_thread::yield();
  }
  if (fake_engine::in_perform.load() == 0) {
    MESSAGE("no lane took the transfer; nothing to check");
  } else {
    { context::file idle(ctx, "fake", {}); }
    CHECK(!watchdog_fired.load());
  }
  done.store(true);
  watchdog.join();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(holds_pattern(got, 0));
}

// A remainder's submission can fail inside a reap, with the transfer
// still owed and the entry never given to the kernel. The failure is
// thrown from that poll, not slept on.
TEST_CASE("a submit failure latched during a reap is thrown, not waited on") {
  context ctx({}, 4);
  ctx.engine().piece = 4096;
  ctx.engine().submits_on_flush = true;
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(2 * 4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.flush();
  ctx.engine().fail_submit = true;
  try {
    (void)ctx.poll(true);
    FAIL("poll() returned with a remainder it could not submit");
  } catch (const std::system_error& e) {
    CHECK(e.code() == std::errc::io_error);
  }
  CHECK(ctx.engine().blocked_unsubmitted == 0);
  CHECK(p.calls == 0);
  ctx.engine().fail_submit = false;
  CHECK(poll_until(ctx, p) == 1);
  CHECK(!p.ec);
  CHECK(holds_pattern(got, 0));
}

// What the kernel owes can end in a control entry -- a cancel's own
// completion, a wake poll -- that the engine consumes without a transfer
// to show for it. The drain finishes on it all the same.
TEST_CASE("a drain whose last debt is a control entry finishes") {
  context ctx({}, 4);
  context::file file(ctx, "fake", {});
  std::vector<std::byte> got(4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  ctx.submit_read(file, 0, std::span(got), op);
  ctx.flush();
  ctx.engine().post_sentinel();
  ctx.drain();
  CHECK(ctx.engine().kernel_owed() == 0);
  CHECK(ctx.in_flight() == 0);
  CHECK(p.calls == 0);
  ctx.engine().post_sentinel();
  ctx.drain();
  CHECK(ctx.engine().kernel_owed() == 0);
}

// Completions that come back together are continued together: their
// remainders go to the kernel in one submission, not one each.
TEST_CASE("the remainders of one reap are submitted together") {
  context ctx({}, 4);
  ctx.engine().piece = 4096;
  ctx.engine().submits_on_flush = true;
  context::file file(ctx, "fake", {});
  std::vector<std::vector<std::byte>> got(4, std::vector<std::byte>(2 * 4096));
  std::vector<probe> p(4);
  std::vector<context::transfer> op(4);
  for (std::size_t i = 0; i < 4; ++i) {
    arm(op[i], p[i]);
    ctx.submit_read(file, i * 2 * 4096, std::span(got[i]), op[i]);
  }
  ctx.flush();
  std::size_t reported = 0;
  for (int polls = 0; reported < 4; ++polls) {
    REQUIRE(polls < 1000);
    reported += ctx.poll(true);
  }
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(p[i].calls == 1);
    CHECK(holds_pattern(got[i], i * 2 * 4096));
  }
  CHECK(ctx.engine().submits == 2);
}

// A wake raised before a poll() is reported by the engine's first,
// non-blocking look, so the poll returns without waiting at all.
TEST_CASE("a wake before the wait ends poll(true) without a wait") {
  context ctx({}, 4);
  ctx.wake();
  CHECK(ctx.poll(true) == 0);
  CHECK(ctx.engine().blocked_waits == 0);
}

// A wake from another thread ends a poll that is already waiting.
TEST_CASE("a wake racing a blocking poll ends it") {
  context ctx({}, 4);
  std::atomic<bool> returned{false};
  std::thread poller([&] {
    (void)ctx.poll(true);
    returned.store(true);
  });
  ctx.wake();
  for (int i = 0; i < 1000 && !returned.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!returned.load()) {
    ctx.wake();  // release it before failing, so join() returns
  }
  poller.join();
  CHECK(returned.load());
}

// A wake a closing file's wait consumed is remembered: the next poll
// returns on it, as it would have had the file not closed first. A
// watchdog wakes the context after ten seconds, so a forgotten wake fails
// here instead of hanging.
TEST_CASE("a wake consumed while a file closes is seen by the next poll") {
  context ctx({}, 4);
  ctx.engine().wake_first = true;
  std::vector<std::byte> got(4096);
  probe p;
  context::transfer op{};
  arm(op, p);
  {
    context::file file(ctx, "fake", {});
    ctx.submit_read(file, 0, std::span(got), op);
    ctx.flush();
    ctx.wake();
  }
  CHECK(p.calls == 0);
  std::atomic<bool> done{false};
  std::atomic<bool> watchdog_fired{false};
  std::thread watchdog([&] {
    for (int i = 0; i < 1000 && !done.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load()) {
      watchdog_fired.store(true);
      ctx.wake();
    }
  });
  CHECK(ctx.poll(true) == 0);
  done.store(true);
  watchdog.join();
  CHECK(!watchdog_fired.load());
  CHECK(ctx.engine().blocked_waits == 0);
}

TEST_CASE("drain() reports nothing and leaves the context usable") {
  context ctx({}, 4);
  context::file file(ctx, "fake", {});
  std::vector<std::byte> a(4096);
  std::vector<std::byte> b(4096);
  probe p;
  context::transfer x{};
  context::transfer y{};
  arm(x, p);
  arm(y, p);
  ctx.submit_read(file, 0, std::span(a), x);
  ctx.submit_read(file, 4096, std::span(b), y);
  ctx.flush();
  ctx.drain();
  CHECK(ctx.in_flight() == 0);
  CHECK(ctx.poll(false) == 0);
  CHECK(p.calls == 0);

  ctx.submit_read(file, 0, std::span(a), x);
  ctx.flush();
  CHECK(poll_until(ctx, p) == 1);
  CHECK(holds_pattern(a, 0));
}

}  // TEST_SUITE
