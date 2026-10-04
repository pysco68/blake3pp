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

#include <blake3pp/detail/ex.hpp>

namespace blake3pp::detail::ex_compat {

// RULE 1 -- completion signatures need both spellings.
//
// stdexec reads a `completion_signatures` member typedef. beman ignores
// it and looks for a static consteval member template
// `get_completion_signatures<Sndr, Env...>()` returning the signatures
// as a value (beman/execution/detail/get_completion_signatures.hpp).
// Declaring both satisfies either; a sender with only the typedef fails
// on beman with "constraints not satisfied for alias template
// 'completion_signatures_of_t'", which does not mention the sender at
// all.
//
// Re-verified against the pinned providers: `let_value` still reproduces
// it, `then` and `when_all` no longer do. One adaptor is enough, so the
// rule stands as written.
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

// RULE 6 -- a namespace-scope template must not hold a provider sender
// unless the operation types are in its own arguments.
//
// stdexec declares its sender expression template inside an anonymous
// namespace (__basic_sender.hpp: `namespace { template <auto
// _DescriptorFn> struct __sexpr ... }`), so every translation unit gets
// its own sender types even for identical template arguments. That is
// deliberate and normally invisible, because a sender type usually
// appears in one place in one function.
//
// It stops being invisible for a class template at namespace scope
// whose members are senders or operation states. Its mangled name comes
// from its own arguments, so two translation units agree on the NAME
// while disagreeing on the LAYOUT, the linker keeps one copy of each
// member function, and one unit's destructor runs over the other
// unit's object. UBSan's vptr check reports it as
//
//     member call on address 0x... which does not point to an object of
//     type '__opstate'
//     note: object is of type '*N7stdexec...__opstate...'
//
// -- the same type name twice, which is the signature of two typeinfos
// for one name.
//
// Function-local types do not have the problem: they are instantiated
// over the anonymous-namespace types and inherit their internal
// linkage. Neither do templates that name the operation types among
// their own arguments, for the same reason. So:
//
//     template <class Scope>                       // WRONG
//     struct cells { std::optional<connect_result_t<...>> cell; };
//
//     template <class Scope, class Op>             // right
//     struct cells { std::optional<Op> cell; };
//     template <class Scope>
//     using cells_for = cells<Scope, connect_result_t<...>>;
//
// The incident: detail::pipeline_cells (file_pipeline.hpp) was written
// the first way so an asynchronous operation state could hold one as a
// member, and the sanitizer lane failed on nine cases. A single
// translation unit was clean either way; tests/provider_linkage.cpp and
// its second translation unit are what reproduce it, and they are in
// the suite so the next one is caught rather than noticed.

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
