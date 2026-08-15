# blake3pp

A C++20-and-later BLAKE3 implementation built as a case study in
hardware-saturating, portable C++26-forward design: `std::simd` and
`std::execution` where the standard library provides them, drop-in polyfills
(xsimd, stdexec) where it doesn't, multi-architecture SIMD kernels compiled
into a single binary with zero-overhead runtime dispatch, and OS-native
direct I/O behind a unified interface.

## Using the library

Everything lives in `namespace blake3pp`, and
`#include <blake3pp/blake3pp.hpp>` gets you all of it. Compile-cost-aware
consumers can pick granular headers instead:

| header                    | provides |
|---------------------------|----------|
| `<blake3pp/blake3pp.hpp>` | umbrella: everything below |
| `<blake3pp/core.hpp>`     | `digest`, `hasher`, one-shot `hash()`, arch introspection |
| `<blake3pp/parallel.hpp>` | multi-core `hash()` and `parallel_hasher` |
| `<blake3pp/io.hpp>`       | `hash_file()`, the async direct-I/O pipeline |

### One-shot hashing

```cpp
#include <blake3pp/blake3pp.hpp>

blake3pp::digest d = blake3pp::hash("hello world");   // string_view

std::vector<std::byte> payload = load_payload();
blake3pp::digest d2 = blake3pp::hash(payload);        // anything span-like

std::cout << d.to_hex() << '\n';                      // lowercase hex
std::cout << std::format("digest: {}\n", d);          // std::format-able
```

`digest` is a regular value type: compare with `==`, round-trip through
hex, use it as a map key after hashing its bytes.

```cpp
// Verification in one line: constant-time comparison, and malformed hex
// is simply "no match".
if (blake3pp::hash(payload).matches(user_input)) { /* verified */ }

// Parsing explicitly? std::optional's heterogeneous == compares the
// contained value (and is false for nullopt); no dereference needed:
if (blake3pp::digest::from_hex(user_input) == blake3pp::hash(payload)) {
  /* verified (ordinary, non-constant-time comparison) */
}
```

### Incremental hashing

`hasher` is a fixed-size value type (no heap, ever). `finalize()` is
non-destructive; you can take a digest mid-stream and keep feeding:

```cpp
blake3pp::hasher h;
while (auto block = source.next_block()) {
  h.update(*block);                         // std::span<const std::byte>
}
blake3pp::digest checkpoint = h.finalize(); // digest of everything so far
h.update(trailer);
blake3pp::digest full = h.finalize();       // ...and of the whole stream
h.reset();                                  // reuse the instance
```

### SIMD variants: introspection and pinning

The binary carries every variant your target platform supports; dispatch
picks the best one at runtime. You can look, and you can override:

```cpp
for (blake3pp::arch a : blake3pp::compiled_arches())   // in this binary
  std::cout << blake3pp::to_string(a) << ' ';
for (blake3pp::arch a : blake3pp::available_arches())  // usable on this CPU
  std::cout << blake3pp::to_string(a) << ' ';          // best-first

blake3pp::hasher pinned{blake3pp::arch::sse42};        // explicit variant
assert(blake3pp::available_arches().front() == blake3pp::best_available());
```

Requesting a variant the CPU can't run silently falls back to the best
available one; `is_available()` tells you beforehand.

### Multi-core hashing (std::execution / stdexec)

One-shot and incremental hashing are each available sequentially or
multi-core; the concepts are orthogonal:

|             | one-shot           | incremental        |
|-------------|--------------------|--------------------|
| sequential  | `hash(data)`       | `hasher`           |
| multi-core  | `hash(data, sched)`| `parallel_hasher`  |

For large in-memory buffers, hand `hash()` any sender/receiver scheduler.
BLAKE3's tree makes the decomposition exact, so the digest is identical to
the sequential one:

```cpp
#include <blake3pp/parallel.hpp>
#include <exec/static_thread_pool.hpp>   // until std::execution ships one

exec::static_thread_pool pool(std::thread::hardware_concurrency());
blake3pp::digest d = blake3pp::hash(big_buffer, pool.get_scheduler());
```

When the data arrives in pieces, `parallel_hasher` has the exact
interface of `hasher`, with the multi-core fan-out (and all of BLAKE3's
subtree-alignment and final-chunk discipline) handled internally:

```cpp
blake3pp::parallel_hasher ph{pool.get_scheduler()};
while (auto block = source.next_block()) {
  ph.update(*block);
}
blake3pp::digest streamed = ph.finalize();   // == the sequential digest

// Tunable, and checkpointable mid-stream just like hasher:
blake3pp::parallel_hasher tuned{pool.get_scheduler(),
                                {.window_bytes = 16 * 1024 * 1024}};
```

### Hashing files at storage speed

`hash_file()` streams the file through fixed windows with io_uring +
`O_DIRECT` on Linux (bypassing the page cache), overlapping reads with
hashing; it degrades gracefully per feature (no O_DIRECT support ->
buffered io_uring -> plain `pread` -> stdio). Paths are
`std::filesystem::path`; every entry point has a throwing form and a
`std::error_code` form, mirroring the standard library:

```cpp
#include <blake3pp/io.hpp>

auto d = blake3pp::hash_file("dataset.parquet");        // throws system_error

std::error_code ec;
auto d2 = blake3pp::hash_file(config.input_path, ec);   // reports via ec
if (ec) { log_error(ec.message()); }

// Full pipeline: async reads + multi-core hashing, tuned:
auto d3 = blake3pp::hash_file(path, pool.get_scheduler(),
                              {.window_bytes = 16 * 1024 * 1024,
                               .queue_depth  = 8});
```

Codebases on Boost.Filesystem work transparently: any path-like type
with a `native()` observer is accepted structurally, so
`blake3pp::hash_file(boost_path)` compiles without blake3pp knowing Boost
exists (and without lossy transcoding on Windows).

### Expert seams

For engines that compute subtrees externally (custom pipelines, the I/O
layer itself), `hasher::push_subtree_cv()` absorbs an
externally-computed subtree chaining value, and `hasher` / the parallel
`hash()` accept a caller-supplied kernel table; that seam is how the
benchmark plugs upstream's hand-written assembly into this pipeline.

### Consuming via CMake

```cmake
include(FetchContent)
FetchContent_Declare(blake3pp GIT_REPOSITORY <this-repo> GIT_TAG main)
FetchContent_MakeAvailable(blake3pp)
target_link_libraries(app PRIVATE blake3pp::blake3pp)
```

The library is C++20; with a C++26 toolchain it uses native `std::simd`,
otherwise `std::experimental::simd` or xsimd: the same source, probed at
configure time. Tests, benchmarks and the `blake3ppsum` CLI only build
when blake3pp is the top-level project.

### Guarantees and caveats

- Compute paths never allocate: `hasher` is a flat value type, subtree
  buffers live on the stack, the I/O pipeline allocates only its buffer
  ring at setup.
- A `hasher` instance is not thread-safe; distinct instances and all free
  functions are.
- Only the I/O layer throws (`std::system_error`, or use the `error_code`
  overloads); compute APIs are `noexcept`.
- Current API scope: plain 256-bit BLAKE3. Keyed hashing, `derive_key`
  and extendable output (XOF) are on the roadmap.

## Building

```bash
cmake --preset linux-gcc16-cxx26 && cmake --build --preset linux-gcc16-cxx26
ctest --preset linux-gcc16-cxx26
```

Any name from `cmake/toolchains/` works as a preset (see
`CMakePresets-toolchains.json`); test presets exist for `linux-gcc16-cxx26`,
`linux-clang22-cxx26`, `linux-clang18-cxx20-libstdcxx` (the C++20 polyfill path), and the
two `-asan` variants. Without a preset, a bare `cmake -S . -B build`
configures the C++20 baseline with the default compiler.

Layout: public API in `include/blake3pp/`, arch-agnostic tree logic in
`src/core/`, the per-architecture kernel (one TU, compiled once per variant
by `cmake/ArchKernels.cmake`) in `src/kernel/`, runtime routing in
`src/dispatch/`. `cmake/StdFeatures.cmake` probes what the active standard
library really ships (by compiling usage, not trusting feature-test macros),
and `tests/` verifies every configuration against the official BLAKE3 test
vectors.

## License

Copyright 2026 Yannic Staudt. Apache License 2.0 with LLVM Exceptions; see
[LICENSE](LICENSE).
