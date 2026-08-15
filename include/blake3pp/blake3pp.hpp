#pragma once

// The umbrella header: one include, the whole library.
//
//   #include <blake3pp/blake3pp.hpp>
//
// Compile-cost-sensitive translation units can include just what they use:
//
//   <blake3pp/core.hpp>      digest, hasher, one-shot hash()
//   <blake3pp/dispatch.hpp>  arch enum, introspection (pulled in by core)
//   <blake3pp/parallel.hpp>  multi-core hash() and parallel_hasher
//                            (brings in the sender/receiver machinery)
//   <blake3pp/io.hpp>        hash_file() and the direct-I/O pipeline
//                            (brings in <filesystem> and parallel.hpp)

#include <blake3pp/core.hpp>
#include <blake3pp/dispatch.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>
