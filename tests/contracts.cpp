// The preconditions the library checks in EVERY build, each shown failing.
//
// These are the contracts whose violation would otherwise be a wrong digest
// or a write past the end of a span, with nothing reported. They throw
// (include/blake3pp/detail/contract.hpp), so they are ordinary cases here:
// what each one promises is the exception type its caller would expect.
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include <blake3pp/blake3pp.hpp>
#include <blake3pp/detail/tree_reducer.hpp>

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

// Two threads inside one context: the second would walk the operation list
// the first is mutating. Only where the file-I/O layer exists: without it
// there is no context to run.
#if defined(BLAKE3PP_WITH_IO)
TEST_CASE("an io_context refuses a second thread that tries to run it") {
  blake3pp::async::io_context ctx({.window_bytes = 64 * 1024});
  std::atomic<bool> running{false};
  std::thread runner([&] {
    running.store(true, std::memory_order_release);
    try {
      ctx.run();
    } catch (...) {  // the stop below is how this one ends
    }
  });
  while (!running.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Long enough that the runner is inside drive() rather than before it.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_THROWS_AS(ctx.run_until_idle(), std::logic_error);
  ctx.request_stop();
  runner.join();
}
#endif

TEST_SUITE_END();
