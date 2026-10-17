#pragma once

// The preconditions the library checks in every build.
//
// Most of its preconditions are assert()s, which is right where a violation
// is something a test or a sanitizer lane would find: an index out of
// range, a null driver, a state machine in the wrong state. The ones that
// come through here are the others -- a caller that inserts the same range
// of a message twice gets a digest that is simply wrong, verifies against
// nothing and reports no error anywhere, and a span written past its end is
// a corruption that surfaces somewhere unrelated. Both vanish under NDEBUG
// if they are only asserted, which is the build that matters.
//
// They are reported as exceptions, not as a message and an abort. The
// library already has a carrier for one: a stage that throws completes its
// chain with set_error(exception_ptr), the scope latches it, and run()
// rethrows it to whoever called update_file(). A caller that broke a
// documented contract gets something it can catch, print and test, and the
// library writes nothing to a stream of its own.
//
// Two places do not use this, and cannot: the io_uring and IOCP drains
// that cannot give back buffers the kernel still owns. Unwinding there IS
// the corruption -- the stack unwinds, the arena is freed, and the kernel
// writes into it -- so those end the process with the reason named.
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
