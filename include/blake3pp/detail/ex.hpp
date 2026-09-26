#pragma once

// The sender/receiver vocabulary, and nothing else.
//
// It is a header of its own because two things need it from opposite
// ends of the include graph: <blake3pp/parallel.hpp>, which is where the
// parallel API lives, and <blake3pp/detail/ex_compat.hpp>, which the
// pipeline's custom senders need. With the vocabulary inside
// parallel.hpp those two could not both be satisfied -- ex_compat would
// include parallel.hpp, and parallel.hpp would include the pipeline that
// includes ex_compat.
//
// Internal to the library, but it does name an execution provider, so
// nothing on the provider-free side of the API (<blake3pp/io.hpp>) may
// include it.

#if defined(BLAKE3PP_EXECUTION_STD)
#include <execution>
#include <stop_token>  // the token types std::execution does not carry
#include <thread>      // this_thread::sync_wait
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
#include <beman/execution/execution.hpp>
#else  // BLAKE3PP_EXECUTION_STDEXEC
#include <stdexec/execution.hpp>
#endif

namespace blake3pp {

/// The sender/receiver vocabulary this build uses (std::execution,
/// beman::execution or stdexec), so the library and its callers spell
/// schedule, bulk and sync_wait the same way whichever provider is built.
namespace ex {
#if defined(BLAKE3PP_EXECUTION_STD)
using namespace std::execution;
using std::this_thread::sync_wait;
// The stop-token vocabulary is the one part of the model the standard
// does not put in std::execution: get_stop_token and stop_token_of_t are
// there, the token types themselves are in <stop_token>, in namespace
// std. Both polyfills keep all of it in one namespace, so these
// declarations are what make the spelling the same everywhere.
using std::inplace_stop_callback;
using std::inplace_stop_source;
using std::inplace_stop_token;
using std::never_stop_token;
using std::stop_callback_for_t;
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
using namespace beman::execution;
#else
using namespace stdexec;
#endif
}  // namespace ex

}  // namespace blake3pp
