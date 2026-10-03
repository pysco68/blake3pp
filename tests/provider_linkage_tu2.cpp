// The SECOND translation unit of the provider-linkage case. Its whole
// job is to instantiate the pipeline's templates over the same
// externally-linked arguments as provider_linkage.cpp, so the two units
// can disagree about a type they both name.
//
// Both entry points matter and they cover different templates:
//
//   hash_file_from_other_tu       the blocking path, which materialises
//                                 the cell store inside window_scope::run()
//   hash_file_async_from_other_tu the async path, which holds one as a
//                                 member of its operation state
//
// Keep this separate and keep both halves: with everything in one
// translation unit there is nothing for the linker to merge, and the
// case passes whether or not the defect is present. See RULE 6 in
// detail/ex_compat.hpp.

#include <filesystem>
#include <utility>

#include <blake3pp/async_io.hpp>
#include <blake3pp/core.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <blake3pp/parallel_io.hpp>

#include "provider_linkage.hpp"

namespace b3test {

blake3pp::digest hash_file_from_other_tu(const std::filesystem::path& path) {
  blake3pp::hasher h;
  blake3pp::update_file(h, path, blake3pp::get_parallel_scheduler());
  return h.finalize();
}

blake3pp::digest hash_file_async_from_other_tu(
    const std::filesystem::path& path) {
  blake3pp::async::io_context ctx({.window_bytes = 64 * 1024,
                                   .queue_depth = 2,
                                   .windows = 2,
                                   .direct_io = false});
  linkage_result out;
  auto op = blake3pp::ex::connect(
      blake3pp::async::async_hash_file(ctx, path,
                                       blake3pp::get_parallel_scheduler()),
      linkage_receiver{&out});
  blake3pp::ex::start(op);
  ctx.run_until_idle();
  return out.value;
}

}  // namespace b3test
