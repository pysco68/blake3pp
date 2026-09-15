// The sized process-wide parallel scheduler
// (<blake3pp/parallel_backend.hpp>).
//
// Its own binary on purpose: every assertion here is about process-global
// state observed in order (nothing has sized it yet, then it is sized,
// then it is running and can no longer be sized), which a shared test
// binary running cases in any order cannot express.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

TEST_CASE("the process scheduler is sized once, before it runs") {
  CHECK(blake3pp::parallel_scheduler_threads() == 0);
  CHECK_THROWS_AS(blake3pp::size_parallel_scheduler(0), std::invalid_argument);
  CHECK(blake3pp::parallel_scheduler_threads() == 0);

  blake3pp::size_parallel_scheduler(2);
  CHECK(blake3pp::parallel_scheduler_threads() == 2);
  // Idempotent while it agrees, an error when it does not.
  blake3pp::size_parallel_scheduler(2);
  CHECK_THROWS_AS(blake3pp::size_parallel_scheduler(3), std::logic_error);
  CHECK(blake3pp::parallel_scheduler_threads() == 2);

  // The scheduler the size applies to still hashes correctly.
  std::vector<std::byte> input(4u << 20);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::byte>(i * 31 + (i >> 8));
  }
  const auto expect = blake3pp::hash(std::span<const std::byte>{input});
  const auto got = blake3pp::hash(std::span<const std::byte>{input},
                                  blake3pp::get_parallel_scheduler());
  CHECK(got == expect);

  // Running now, so the size is settled for the rest of the process.
  CHECK_THROWS_AS(blake3pp::size_parallel_scheduler(4), std::logic_error);
  CHECK(blake3pp::parallel_scheduler_threads() == 2);
}
