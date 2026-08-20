#pragma once

// The one platform decision left in the I/O layer. Each branch names the
// backend pair the OS gets; everything else (the engines and the backends
// themselves) is straight-line C++. The engine TUs static_assert the
// concepts (io/backend.hpp) against these aliases, so a backend drifting
// from the contract fails loudly at this seam, not somewhere inside the
// engine. Internal to src/io/, never installed.

#if defined(__linux__)
#include "io/uring_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader = uring_reader;
using native_writer = uring_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(_WIN32)
#include "io/iocp_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader = iocp_reader;
using native_writer = iocp_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(__APPLE__)
#include "io/gcd_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader = gcd_reader;
using native_writer = gcd_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(__unix__)
#include "io/pread_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader = pread_reader;
using native_writer = pread_writer;
}  // namespace blake3pp::detail::io_impl
#else
#include "io/stdio_backend.hpp"
namespace blake3pp::detail::io_impl {
using native_reader = stdio_reader;
using native_writer = stdio_writer;
}  // namespace blake3pp::detail::io_impl
#endif
