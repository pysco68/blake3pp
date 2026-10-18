#pragma once

// The preconditions the library checks in every build.
//
// Most preconditions are assert()s: an index out of range, a null driver, a
// state machine in the wrong state -- violations a test or a sanitizer lane
// finds. The ones that come through here are the others, whose consequence
// under NDEBUG is silent. A range of the message inserted twice produces a
// digest that is simply wrong, verifies against nothing and reports no
// error anywhere; a span written past its end corrupts memory that surfaces
// somewhere unrelated.
//
// They are reported as exceptions, which the pipeline already carries: a
// stage that throws completes its chain with set_error(exception_ptr), the
// scope latches it, and run() rethrows it to whoever called update_file().
//
// The io_uring and IOCP drains cannot use this, per file or per context:
// they cannot give back buffers the kernel still owns, and unwinding frees
// the arena the kernel is about to write into. Those end the process with
// the reason named.
#include <stdexcept>
#include <string>

namespace blake3pp::detail {

// `what` names the broken contract in the terms the caller broke it:
// "tree_reducer::insert(): a range overlapping one already inserted".
// Ex is the exception a caller would expect of the function it called --
// std::length_error for a span too small, std::invalid_argument for a
// value out of range -- and a library invariant that cannot be a caller's
// mistake stays on the default.
template <class Ex = std::logic_error>
[[noreturn]] void contract_broken(const char* what) {
  throw Ex(std::string("blake3pp: ") + what);
}

// Checked in every build. The condition is the contract as the caller must
// keep it, so a reader sees what was promised rather than what failed.
template <class Ex = std::logic_error>
void require(bool contract_holds, const char* what) {
  if (!contract_holds) [[unlikely]] {
    contract_broken<Ex>(what);
  }
}

}  // namespace blake3pp::detail
