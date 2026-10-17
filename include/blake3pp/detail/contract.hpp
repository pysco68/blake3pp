#pragma once

// The one place the library ends the process on a broken precondition.
//
// Most of its preconditions are assert()s, which is right where a
// violation is something a test or a sanitizer lane would find: an index
// out of range, a null driver, a state machine in the wrong state. The
// ones here are the others -- a caller that inserts the same range of a
// message twice gets a digest that is simply wrong, verifies against
// nothing and reports no error anywhere, and a span written past its end
// is a corruption that surfaces somewhere unrelated. Both vanish under
// NDEBUG if they are only asserted, which is the build that matters.
//
// Returning the failure is not available: the functions that have these
// preconditions are noexcept, and several of them are deep inside a
// completion where there is nobody to return it to. So the process ends
// with the reason named, which is the answer the io_uring backend already
// gives when it cannot release buffers the kernel holds.
#include <cstdio>
#include <cstdlib>

namespace blake3pp::detail {

// `what` names the broken contract, in the terms the caller broke it:
// "tree_reducer::insert(): a range overlapping one already inserted".
[[noreturn]] inline void contract_violation(const char* what) noexcept {
  std::fprintf(stderr, "blake3pp: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

// Checked in every build. The condition is the contract as the caller must
// keep it, so a reader sees what was promised rather than what failed.
inline void require(bool contract_holds, const char* what) noexcept {
  if (!contract_holds) [[unlikely]] {
    contract_violation(what);
  }
}

}  // namespace blake3pp::detail
