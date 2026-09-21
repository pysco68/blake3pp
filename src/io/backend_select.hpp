#pragma once

// The one platform decision left in the I/O layer. Each branch names the
// reader ENGINE the OS gets and the writer backend, and everything else
// (the engines and the backends themselves) is straight-line C++. The
// engine TUs static_assert the concepts (io/backend.hpp) against these
// aliases, so a backend drifting from the contract fails loudly at this
// seam, not somewhere inside the engine.
//
// The reader side names a whole engine rather than a backend, which is
// what lets a platform bring its own: every one of them now runs
// polled_reader_engine over a reader_context, and the alias is the seam
// where a future engine could differ again. Internal to src/io/, never
// installed.

#include "io/engine.hpp"

#if defined(__linux__)
#include "io/uring_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader_engine = polled_reader_engine<uring_context>;
using native_writer = uring_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(_WIN32)
#include "io/iocp_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader_engine = polled_reader_engine<iocp_context>;
using native_writer = iocp_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(__APPLE__)
#include "io/gcd_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader_engine = polled_reader_engine<gcd_context>;
using native_writer = gcd_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(__unix__)
#include "io/pread_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader_engine = polled_reader_engine<pread_context>;
using native_writer = pread_writer;
}  // namespace blake3pp::detail::io_impl
#else
#include "io/stdio_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader_engine = polled_reader_engine<stdio_context>;
using native_writer = stdio_writer;
}  // namespace blake3pp::detail::io_impl
#endif
