#pragma once

// The one platform decision left in the I/O layer. Each branch names the
// io_context the OS gets, and everything else is straight-line C++. Each
// per-OS header static_asserts the concept (io/io_context.hpp) against
// its context, and polled_reader and polled_writer are constrained by it,
// so a context drifting from the contract fails at its definition, not
// somewhere inside the window machine.
//
// Every platform runs polled_reader and polled_writer over its context,
// so their aliases are written once, after the branches. The context is
// named on its own because detail::io_driver (the file pipeline's
// compiled seam) drives one directly, with neither over it.
// Internal to src/io/, never installed.

#include "io/polled_io.hpp"

#if defined(__linux__)
#include "io/linux/io_uring.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = uring_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(_WIN32)
#include "io/windows/engine.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = windows_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(__APPLE__)
#include "io/macos/dispatch.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = gcd_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(__FreeBSD__)
#include "io/freebsd/aio.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = aio_context;
}  // namespace blake3pp::detail::io_impl
#elif defined(__unix__)
#include "io/posix/pread.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = pread_context;
}  // namespace blake3pp::detail::io_impl
#else
#include "io/portable/stdio.hpp"
namespace blake3pp::detail::io_impl {
using native_io_context = stdio_context;
}  // namespace blake3pp::detail::io_impl
#endif

namespace blake3pp::detail::io_impl {
using native_reader = polled_reader<native_io_context>;
using native_writer = polled_writer<native_io_context>;
}  // namespace blake3pp::detail::io_impl
