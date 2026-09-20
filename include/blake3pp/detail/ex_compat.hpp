#pragma once

// What a custom sender, scheduler or receiver has to spell differently
// per sender/receiver provider, in one place. Every custom sender in the
// file pipeline goes through this header; each rule below names the
// provider that needs it and what it does when the rule is broken,
// because the diagnostics are long and none of them say "you are missing
// a static member".
//
// Measured on stdexec (the repo's polyfill) and beman.execution. The
// std::execution path is unverified: no toolchain in the matrix has
// <execution> senders yet, which tests/file_pipeline.cpp asserts loudly
// rather than assuming.
//
// Internal to the pipeline; <blake3pp/io.hpp> must not reach this, since
// it would drag the provider into the public I/O surface.

#include <blake3pp/parallel.hpp>

namespace blake3pp::detail::ex_compat {

// RULE 1 -- completion signatures need both spellings.
//
// stdexec reads a `completion_signatures` member typedef. beman ignores
// it and looks for a static consteval member template
// `get_completion_signatures<Sndr, Env...>()` returning the signatures
// as a value (beman/execution/detail/get_completion_signatures.hpp).
// Declaring both satisfies either; a sender with only the typedef fails
// on beman inside `let`/`then` with "constraints not satisfied for alias
// template 'completion_signatures_of_t'", which does not mention the
// sender at all.
#define BLAKE3PP_EX_COMPLETION_SIGNATURES(...)                        \
  using completion_signatures = ::blake3pp::ex::completion_signatures<__VA_ARGS__>;  \
  template <class Sndr, class... Env>                                 \
  static consteval auto get_completion_signatures() {                 \
    return ::blake3pp::ex::completion_signatures<__VA_ARGS__>{};      \
  }                                                                   \
  static_assert(true) /* force a semicolon at the use site */

// RULE 2 -- complete receivers through the CPO, never the member.
//
//     ex::set_value(std::move(rcvr), value);   // yes
//     std::move(rcvr).set_value(value);        // no
//
// beman's internal basic_receiver keeps set_value private and reachable
// only through the CPO, so the member spelling fails with "'set_value'
// is a private member of beman::execution::detail::basic_receiver<...>".
// stdexec accepts either, which is what makes the mistake survive a
// stdexec-only build.

// RULE 3 -- a scheduler must answer get_forward_progress_guarantee.
//
// beman's scheduler concept requires it and supplies no default; a
// scheduler without it is simply not a scheduler, and the symptom is
// `continues_on` failing to match with no mention of the query. Inherit
// this into any scheduler the library defines. weakly_parallel is the
// honest answer for a run loop: it advances only while its owner drives
// it.
struct weakly_parallel_scheduler {
  [[nodiscard]] ::blake3pp::ex::forward_progress_guarantee query(
      ::blake3pp::ex::get_forward_progress_guarantee_t) const noexcept {
    return ::blake3pp::ex::forward_progress_guarantee::weakly_parallel;
  }
  // A scheduler must be equality-comparable, and a derived class's
  // defaulted operator== compares its bases: without this one the
  // derived scheduler silently stops being a scheduler.
  bool operator==(const weakly_parallel_scheduler&) const noexcept = default;
};

// RULE 4 -- the tag types have two spellings.
//
// beman renamed receiver_t/sender_t/... to *_tag and left the old names
// as deprecated aliases, which warn under -Wdeprecated-declarations.
// stdexec has only the _t names. Use these aliases and neither warns.
#if defined(BLAKE3PP_EXECUTION_BEMAN)
using receiver_tag = ::beman::execution::receiver_tag;
using sender_tag = ::beman::execution::sender_tag;
using operation_state_tag = ::beman::execution::operation_state_tag;
using scheduler_tag = ::beman::execution::scheduler_tag;
#else
using receiver_tag = ::blake3pp::ex::receiver_t;
using sender_tag = ::blake3pp::ex::sender_t;
using operation_state_tag = ::blake3pp::ex::operation_state_t;
using scheduler_tag = ::blake3pp::ex::scheduler_t;
#endif

// RULE 5 -- a receiver must not be an aggregate.
//
// An adaptor that stores a receiver by value initialises it from its own
// wrapper type; an aggregate receiver then offers its first member as a
// conversion target and the error names that member rather than the
// receiver. Give every receiver a real constructor.
//
// And it must stay COPYABLE: beman's continues_on stores the receiver by
// copy from an lvalue (continues_on.hpp, state_type), so a move-only
// receiver cannot pass through it there. The pipeline's receivers are
// handles -- a window pointer and a scope pointer -- which is why this
// costs nothing.

// A receiver that accepts every completion, for the concept check each
// custom sender and operation state ends with. An operation state is
// only a type once a receiver is named, so the check needs one, and a
// receiver written for that purpose is exactly where RULE 5's aggregate
// trap gets sprung: hence the user-provided constructor.
struct probe_receiver {
  using receiver_concept = receiver_tag;
  probe_receiver() noexcept {}
  template <class... Values>
  void set_value(Values&&...) && noexcept {}
  template <class Error>
  void set_error(Error&&) && noexcept {}
  void set_stopped() && noexcept {}
};

}  // namespace blake3pp::detail::ex_compat
