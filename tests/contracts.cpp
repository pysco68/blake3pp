// The preconditions the library checks in EVERY build, each shown failing.
//
// These are the contracts whose violation would otherwise be a wrong digest
// or a write past the end of a span, with nothing reported. They throw
// (include/blake3pp/detail/contract.hpp), so they are ordinary cases here:
// what each one promises is the exception type its caller would expect.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <blake3pp/blake3pp.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/memory_driver.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/parallel_backend.hpp>

namespace {

blake3pp::detail::tree_reducer::cv_type some_cv(std::uint32_t seed) {
  blake3pp::detail::tree_reducer::cv_type cv{};
  for (std::size_t i = 0; i < cv.size(); ++i) {
    cv[i] = seed * 2654435761u + static_cast<std::uint32_t>(i);
  }
  return cv;
}

struct reducer_fixture {
  blake3pp::hasher h{};
  std::array<blake3pp::detail::tree_reducer::node, 64> storage{};
  blake3pp::detail::tree_reducer r{
      blake3pp::detail::resolve(h.selected_arch()), h.key_words(),
      h.mode_flags(), storage};
};

}  // namespace

TEST_SUITE_BEGIN("contracts");

// Absorbing the same chunks twice: the digest would cover a message that
// does not exist and verify against nothing. Both shapes: a range whose
// sibling has already merged it away, and a duplicate that fits inside a
// hole in the covered span, which totals alone cannot tell from a new range.
TEST_CASE("the reducer refuses a range it has already absorbed") {
  reducer_fixture f;
  const auto cv = some_cv(1);
  CHECK(f.r.insert(0, 4, cv));
  CHECK(f.r.insert(4, 4, cv));  // 0..8 now, as one node
  CHECK_THROWS_AS((void)f.r.insert(0, 4, cv), std::logic_error);
}

TEST_CASE("the reducer refuses a duplicate that fits in a hole") {
  reducer_fixture f;
  CHECK(f.r.insert(0, 1, some_cv(1)));
  CHECK(f.r.insert(4, 1, some_cv(2)));  // chunks 1..4 are still missing
  CHECK_THROWS_AS((void)f.r.insert(4, 1, some_cv(2)), std::logic_error);
}

// A hole in the run puts every node after it at the wrong position.
TEST_CASE("the reducer refuses to drain a run with a hole") {
  reducer_fixture f;
  CHECK(f.r.insert(0, 1, some_cv(1)));
  CHECK(f.r.insert(2, 1, some_cv(2)));  // chunk 1 is missing
  CHECK_THROWS_AS(f.r.drain_into(f.h), std::logic_error);
}

// A run that does not start where the hasher stands is the same mistake
// one window earlier.
TEST_CASE("the reducer refuses to drain a run that starts elsewhere") {
  reducer_fixture f;
  CHECK(f.r.insert(4, 1, some_cv(3)));
  CHECK_THROWS_AS(f.r.drain_into(f.h), std::logic_error);
}

// The expert entry point's arguments, which divide by zero or index the
// chaining-value stack out of bounds.
TEST_CASE("push_subtree_cv refuses a size or a position it cannot have") {
  blake3pp::hasher h;
  CHECK_THROWS_AS(h.push_subtree_cv(some_cv(1), 0), std::invalid_argument);
  CHECK_THROWS_AS(h.push_subtree_cv(some_cv(1), 3), std::invalid_argument);

  // And a hasher part-way through a chunk: those bytes would be hashed as
  // the head of a chunk past the subtree.
  blake3pp::hasher mid;
  const std::vector<std::byte> partial(100, std::byte{0x11});
  mid.update(partial);
  CHECK_THROWS_AS(mid.push_subtree_cv(some_cv(2), 2), std::invalid_argument);
}

// to_hex() writes two characters per byte whatever the span says.
TEST_CASE("to_hex refuses an output span that cannot hold the hex") {
  const blake3pp::digest d = blake3pp::hash("abc");
  std::array<char, 16> too_small{};
  CHECK_THROWS_AS(blake3pp::to_hex(d.bytes, too_small), std::length_error);
  std::array<char, 64> exact{};
  CHECK_NOTHROW(blake3pp::to_hex(d.bytes, exact));
}

// An odd count would drop the chaining value that had no sibling.
TEST_CASE("fold_sibling_cvs refuses a count that is not a power of two") {
  blake3pp::hasher h;
  std::array<blake3pp::detail::tree_reducer::cv_type, 3> three{
      some_cv(1), some_cv(2), some_cv(3)};
  blake3pp::detail::tree_reducer::cv_type out{};
  CHECK_THROWS_AS(
      blake3pp::detail::fold_sibling_cvs(
          blake3pp::detail::resolve(h.selected_arch()), three, h.key_words(),
          h.mode_flags(), out),
      std::logic_error);
}

// A span that does not end the message goes to the pool as whole
// subtrees, so one that is not a whole number of windows is refused
// before any byte is hashed, in every build.
TEST_CASE("a pipeline span that does not end the message must be whole windows") {
  constexpr std::size_t w = 64 * 1024;
  const std::vector<std::byte> bytes(2 * w + 1, std::byte{0x5a});
  for (const std::size_t len : {2 * w, 2 * w + 1}) {
    CAPTURE(len);
    const std::span<const std::byte> span = std::span{bytes}.first(len);
    blake3pp::hasher h;
    blake3pp::detail::memory_driver drv;
    blake3pp::detail::memory_driver::file f(drv, span);
    blake3pp::detail::pipeline_plan plan =
        blake3pp::detail::plan_memory_pipeline(h.count(), w, len);
    REQUIRE(plan.window_bytes == w);
    plan.ends_message = false;
    const auto run = [&] {
      blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
          h, drv, f, blake3pp::get_parallel_scheduler(), plan,
          {.window_bytes = w});
    };
    if (len % w == 0) {
      CHECK_NOTHROW(run());
      CHECK(h.count() == len);
    } else {
      CHECK_THROWS_AS(run(), std::logic_error);
      CHECK(h.count() == 0);
    }
  }
}

// Two threads inside one context: the second would walk the operation list
// the first is mutating. Only where the file-I/O layer exists: without it
// there is no context to run.
#if defined(BLAKE3PP_WITH_IO)
TEST_CASE("an io_context refuses a second thread that tries to run it") {
  // Both threads retry until the runner is inside run(), where it stays
  // until the stop, and this thread is refused. A run_until_idle() that
  // gets in first returns at once, since nothing is started, and a runner
  // refused meanwhile tries again.
  blake3pp::async::io_context ctx({.window_bytes = 64 * 1024});
  std::thread runner([&] {
    for (;;) {
      try {
        ctx.run();  // until the request_stop() below
        return;
      } catch (const std::logic_error&) {
        std::this_thread::yield();  // this thread held it for a moment
      }
    }
  });
  bool refused = false;
  while (!refused) {
    try {
      ctx.run_until_idle();  // nothing started, so it returns at once
      std::this_thread::yield();
    } catch (const std::logic_error&) {
      refused = true;
    }
  }
  CHECK(refused);
  ctx.request_stop();
  runner.join();
}
#endif

TEST_SUITE_END();
