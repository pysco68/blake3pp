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

// Allocation-free hex for hot paths and C interop: 64 chars, NUL-terminated.
std::array<char, 65> hex = d.to_hex_chars();
std::printf("%s\n", hex.data());
```

`digest` is a regular value type: compare with `==`, round-trip through
hex, use it as a map key after hashing its bytes.

```cpp
// Verification in one line: constant-time comparison, and malformed hex
// is simply "no match".
if (blake3pp::hash(payload).matches(user_input)) { /* verified */ }

// Parsing explicitly? std::optional's heterogeneous == compares the
// contained value (and is false for nullopt); no dereference needed.
// digest's operator== is itself constant-time (the safe default for a
// crypto value type, matching the Rust reference):
if (blake3pp::digest::from_hex(user_input) == blake3pp::hash(payload)) {
  /* verified */
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

### Keyed hashing and key derivation

BLAKE3's keyed mode is its built-in MAC/PRF (the modern replacement for
HMAC). Keys are exactly 32 bytes; the span extent makes a wrong-sized key
a compile error:

```cpp
std::array<std::byte, 32> key = load_secret_key();

blake3pp::digest mac = blake3pp::keyed_hash(key, message);

blake3pp::hasher h = blake3pp::hasher::keyed(key);   // incremental MAC
h.update(header);
h.update(body);
blake3pp::digest tag = h.finalize();
```

`derive_key` is the domain-separated KDF: derive purpose-bound subkeys
from one master secret, tied to a hardcoded, application-unique context
string (the context is not a secret; it is what keeps unrelated uses of
the same key material cryptographically independent):

```cpp
auto session_key =
    blake3pp::derive_key("example.com 2026-08 tls session", master_secret);
auto storage_key =
    blake3pp::derive_key("example.com 2026-08 disk encryption", master_secret);
```

Both modes compose with everything else: `hasher::keyed`/
`hasher::derive_key` give incremental hashing, `keyed_hash(key, data,
sched)` and a keyed `parallel_hasher` constructor go multi-core, and
`hash_file`'s options take a key, for authenticated file manifests at
full pipeline speed:

```cpp
auto tag = blake3pp::hash_file(path, pool.get_scheduler(), {.key = key});
```

### Extended output (XOF)

BLAKE3 is natively an extendable-output function: the 32-byte digest is
just the first 32 bytes of an unbounded stream. Ask for any length, or
take the seekable reader:

```cpp
blake3pp::hasher h;
h.update(seed_material);

std::array<std::byte, 64> wide_key;
h.finalize(wide_key);                       // any output length

blake3pp::output_reader r = h.finalize_xof();
r.fill(first_chunk);                        // stream sequentially...
r.seek(10'000'000'000);                     // ...or jump: O(1) random access
r.fill(deep_chunk);                         // byte 10 GB costs same as byte 0
```

Extended output works in all three modes (plain, keyed, derive_key): a
keyed hasher's `finalize_xof()` streams the MAC'd output, and
`derive_key` hashers can emit subkeys of any width.

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

On AVX-512 machines one more dial exists: the 16-lane message transpose
has three implementation strategies whose ranking depends on whether the
CPU's AVX-512 datapath is full-width or double-pumped, a property no
CPUID bit reports. The default (`quartered`) is the measured best, and
`tune_transpose16()` settles it empirically on the running machine
(~1 ms race, applies the winner process-wide):

```cpp
blake3pp::transpose16 best = blake3pp::tune_transpose16();
std::cout << "transpose strategy: " << blake3pp::to_string(best) << '\n';
```

The build configuration is introspectable too, which is handy for
diagnostics banners and bug reports:

```cpp
std::cout << std::format("blake3pp {} (simd: {}, execution: {})\n",
                         blake3pp::version(), blake3pp::simd_provider(),
                         blake3pp::execution_provider());
// e.g. "blake3pp 0.1.0 (simd: std::simd, execution: stdexec)"
```

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

`hash_file()` streams the file through fixed windows using the fastest
OS-native mechanism (io_uring + `O_DIRECT` on Linux, IOCP +
`FILE_FLAG_NO_BUFFERING` on Windows), bypassing the page cache and
overlapping reads with hashing; it degrades gracefully per feature
(no direct I/O -> buffered async -> plain synchronous reads -> stdio). Paths are
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

### Command-line tools

Two utilities build alongside the library (top-level builds only):

**`blake3ppsum`** is a `sha1sum`-style checksum tool covering the full
spec: `--keyed FILE` (MAC mode; key as 32 raw bytes or 64 hex chars,
never on the command line), `--derive-key CONTEXT`, `--length N`
(extended output), plus `--check` verification, `--arch`/`--threads`, and
the I/O pipeline knobs. `--version` reports the providers and SIMD
variants baked into the binary.

```bash
blake3ppsum big.iso                          # multi-core, direct-I/O
blake3ppsum --keyed key.hex manifest/* > sums && blake3ppsum --keyed key.hex -c sums
blake3ppsum --derive-key "backup 2026 v1" --length 64 master.key
```

**`blake3ppgen`** is a deterministic, *seekable* byte-stream generator
built on extended output: the same seed always yields the same infinite
stream, and `--seek` is O(1), so materializing a slice at offset 10 GB
costs the same as offset 0. Generation runs lanes-parallel in the kernel
(~3.8 GiB/s per core) and `--threads` fans segments across cores via the
O(1) seek (13+ GiB/s), so the sink is the bottleneck; `--output`
removes even that overhead, writing through io_uring + O_DIRECT on
Linux or IOCP + no-buffering on Windows with the stream generated
straight into the write buffers, bypassing the page cache entirely:

```bash
blake3ppgen --seed run42 --length 1G > testdata.bin
blake3ppgen --seed run42 --seek 10G --length 1M > slice.bin   # instant
blake3ppgen --seed run42 --length 100G --threads 0 \
            --output fixture.bin                              # device-bound
```

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
- Full BLAKE3 spec surface: plain, keyed and derive_key modes, each with
  arbitrary-length (XOF) output; every mode is verified against all 131
  output bytes of the official test vectors.

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
