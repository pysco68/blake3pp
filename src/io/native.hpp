#pragma once

// The one platform decision left in the I/O layer. Each branch names the
// io_context the OS gets, and everything else (the engines and the
// backends themselves) is straight-line C++. The engine TUs
// static_assert the concepts (io/io_context.hpp) against these aliases, so a
// backend drifting from the contract fails loudly at this seam, not
// somewhere inside the engine.
//
// Every platform runs polled_reader_engine and polled_writer_engine over
// its context, so the engine aliases are written once, after the
// branches; a platform that brought its own engine would branch there.
// The context is named on its own because detail::io_driver (the file
// pipeline's compiled seam) drives one directly, without an engine over
// it.
// Internal to src/io/, never installed.

#include "io/engine.hpp"

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
using native_reader_engine = polled_reader_engine<native_io_context>;
using native_writer_engine = polled_writer_engine<native_io_context>;
}  // namespace blake3pp::detail::io_impl
