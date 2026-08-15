#include <cstddef>
#include <thread>
#include <vector>

#include <blake3pp/parallel.hpp>
#include <doctest/doctest.h>

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
#include <exec/static_thread_pool.hpp>
#endif

namespace {

std::vector<std::byte> make_input(std::size_t len) {
  std::vector<std::byte> v(len);
  for (std::size_t i = 0; i < len; ++i) {
    v[i] = static_cast<std::byte>(i % 251);
  }
  return v;
}

TEST_SUITE("parallel") {

// The parallel decomposition must be invisible: identical digests to the
// sequential path at every size class (below the parallel threshold, right
// at partition boundaries, and deep multi-level trees).
TEST_CASE("parallel hash matches sequential at every size class") {
#if defined(BLAKE3PP_HAS_STD_SENDERS)
  auto sched = std::execution::get_system_scheduler();
#else
  exec::static_thread_pool pool(std::thread::hardware_concurrency());
  auto sched = pool.get_scheduler();
#endif

  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{1024}, std::size_t{1025},
        std::size_t{31 * 1024}, std::size_t{32 * 1024},
        std::size_t{32 * 1024 + 1}, std::size_t{1024 * 1024},
        std::size_t{(4 * 1024 + 3) * 1024 + 17},
        std::size_t{32 * 1024 * 1024 + 1023}}) {
    CAPTURE(len);
    const auto input = make_input(len);
    CHECK(blake3pp::hash(input, sched) == blake3pp::hash(input));
  }
}

TEST_CASE("parallel hash is deterministic across runs") {
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
  exec::static_thread_pool pool(std::thread::hardware_concurrency());
  auto sched = pool.get_scheduler();
  const auto input = make_input(8 * 1024 * 1024 + 7);
  const auto first = blake3pp::hash(input, sched);
  for (int r = 0; r < 5; ++r) {
    CHECK(blake3pp::hash(input, sched) == first);
  }
#endif
}

}  // TEST_SUITE

}  // namespace
