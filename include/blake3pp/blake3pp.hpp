#pragma once

/// @file
/// The umbrella header: one include, the whole library.
///
/// @code
/// #include <blake3pp/blake3pp.hpp>
/// @endcode
///
/// Compile-cost-sensitive translation units can include just what they
/// use:
///
/// @code{.unparsed}
/// <blake3pp/core.hpp>        digest, hasher, one-shot hash()
/// <blake3pp/dispatch.hpp>    arch enum, introspection (pulled in by core)
/// <blake3pp/parallel.hpp>    multi-core hash(), parallel_hasher, XOF fill()
///                            (brings in the sender/receiver machinery)
/// <blake3pp/io.hpp>          update_file(), hash_file() and the direct-I/O
///                            pipeline (brings in <filesystem>; standard
///                            library only)
/// <blake3pp/parallel_io.hpp> the same over a scheduler (io.hpp +
///                            parallel.hpp)
/// <blake3pp/trace.hpp>       per-window timing records for update_file()
///                            (standard library only)
/// @endcode
///
/// The I/O headers follow the BLAKE3PP_WITH_IO build option: a
/// freestanding build has no filesystem to offer them.

#include <blake3pp/core.hpp>
#include <blake3pp/dispatch.hpp>
#include <blake3pp/parallel.hpp>
#if BLAKE3PP_WITH_IO
#include <blake3pp/io.hpp>
#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>
#endif
