// A regression case for RULE 6 in detail/ex_compat.hpp: a template at
// namespace scope must not hold a provider sender unless the operation
// types are named in its own arguments.
//
// stdexec declares its sender expression template inside an anonymous
// namespace, so every translation unit gets its own sender types. A
// template parameterised only on externally-linked arguments then has
// ONE mangled name over per-unit layouts, the linker keeps one copy of
// each member function, and one unit's destructor runs over the other
// unit's object.
//
// That is why this case is two translation units on purpose. Both drive
// the same pipeline over the same scope type: this one through the
// async senders, provider_linkage_tu2.cpp through both the async
// senders and the blocking entry point. Either unit alone is clean --
// there is nothing to merge -- and the pair is what reproduces it.
//
// The failure is not a crash and not a wrong digest: both halves give
// the right answer either way. It shows up only under the sanitizer
// lanes, as
//
//     runtime error: member call on address 0x... which does not point
//     to an object of type '__opstate'
//
// so this case earns its place by being compiled and run there, not by
// what it asserts.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <blake3pp/async_io.hpp>
#include <blake3pp/core.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <doctest/doctest.h>

#include "provider_linkage.hpp"

namespace {
namespace fs = std::filesystem;
namespace async = blake3pp::async;
namespace ex = blake3pp::ex;

// b3test::linkage_receiver rather than one declared here: a receiver in
// this anonymous namespace would give the operation state internal
// linkage on its own, and the case would pass either way.
[[nodiscard]] blake3pp::digest hash_async_here(const fs::path& path) {
  async::io_context ctx({.window_bytes = 64 * 1024,
                         .queue_depth = 2,
                         .windows = 2,
                         .direct_io = false});
  b3test::linkage_result out;
  auto op = ex::connect(
      async::async_hash_file(ctx, path, blake3pp::get_parallel_scheduler()),
      b3test::linkage_receiver{&out});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(out.completions == 1);
  return out.value;
}

}  // namespace

TEST_SUITE("provider_linkage") {

TEST_CASE("the async and blocking pipelines coexist across translation units") {
  std::vector<std::byte> content(3 * 64 * 1024 + 313);
  std::mt19937 rng(4242);
  for (std::byte& b : content) {
    b = static_cast<std::byte>(rng() & 0xffu);
  }
  // A name of its own: this suite runs on every platform, so nothing
  // POSIX-only goes into it.
  std::random_device entropy;
  const fs::path path =
      fs::temp_directory_path() /
      ("blake3pp_linkage_" + std::to_string(entropy()) + ".bin");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
  }
  const blake3pp::digest want = blake3pp::hash(content);

  // This unit's async operation state, built over an externally linked
  // receiver -- the same one the other unit uses below.
  CHECK(hash_async_here(path) == want);

  // The other unit, blocking: the cell store inside window_scope::run().
  CHECK(b3test::hash_file_from_other_tu(path) == want);

  // The other unit, async: the same operation state type as
  // hash_async_here above, instantiated a second time. This is the pair
  // that has to agree.
  CHECK(b3test::hash_file_async_from_other_tu(path) == want);

  // Once more from here, so a destructor from either unit has run over
  // an object the other one's code could have built.
  CHECK(hash_async_here(path) == want);

  std::error_code ec;
  fs::remove(path, ec);
}

}  // TEST_SUITE
