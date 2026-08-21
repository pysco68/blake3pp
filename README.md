# blake3pp

A C++20-and-later BLAKE3 implementation built as a case study in
hardware-saturating, portable C++26-forward design: `std::simd` and
`std::execution` where the standard library provides them, drop-in polyfills
(xsimd, stdexec) where it doesn't, multi-architecture SIMD kernels compiled
into a single binary with zero-overhead runtime dispatch, and OS-native
direct I/O behind a unified interface.

## Using the library

blake3pp is normally consumed **from source**: vendor it with FetchContent
(or a submodule + `add_subdirectory`) and link the target; tests, benchmarks
and tools stay out of your build automatically:

```cmake
include(FetchContent)
FetchContent_Declare(blake3pp
  GIT_REPOSITORY https://github.com/pysco68/blake3pp.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(blake3pp)
target_link_libraries(your_app PRIVATE blake3pp::blake3pp)
```

The polyfill dependencies (xsimd, stdexec) are fetched and version-pinned
by blake3pp's own build where the toolchain lacks the C++26 facilities;
nothing to install. Building from source is recommended at 0.x because the
feature probes run against *your* toolchain and standard-library
combination; an installed package freezes the choices made on the machine
that built it. Installing is supported too, see
[Installing and find_package](#installing-and-find_package).

For the **tools** (blake3ppsum, blake3ppgen, benchmarks), mimalloc is
the default allocator on every supported target (`BLAKE3PP_TOOL_MIMALLOC`,
off only for wasm): full malloc override on POSIX, global operator
new/delete override on Windows (the reliable static route there), keeping
single-file executables. The library itself stays allocator-neutral.
Binary releases are fully static Linux executables (x86_64 + aarch64,
musl + mimalloc, built via `tools/make-release.sh` with the zig toolchain
presets): no glibc version coupling, no dynamic loader, runtime SIMD
dispatch intact, one file that runs on any distro.

Everything lives in `namespace blake3pp`, and
`#include <blake3pp/blake3pp.hpp>` gets you all of it. Compile-cost-aware
consumers can pick granular headers instead:

| header                       | provides | needs an execution provider |
|------------------------------|----------|:---:|
| `<blake3pp/blake3pp.hpp>`    | umbrella: everything below | yes |
| `<blake3pp/dispatch.hpp>`    | `arch` introspection, SIMD variant selection | no |
| `<blake3pp/core.hpp>`        | `digest`, `hasher`, one-shot `hash()` | no |
| `<blake3pp/io.hpp>`          | `update_file()` and `hash_file()`, the async direct-I/O pipeline | no |
| `<blake3pp/parallel.hpp>`    | multi-core `hash()` and `parallel_hasher` | yes |
| `<blake3pp/parallel_io.hpp>` | `update_file()` and `hash_file()` over a scheduler (the two combined) | yes |

The last column is the one that matters when you install blake3pp rather
than build it: only the scheduler-taking headers include an execution
library (stdexec, beman.execution, or `<execution>`). Hashing buffers and
hashing files sequentially compile against the standard library alone.

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
sched)` and a keyed `parallel_hasher` constructor go multi-core, and a
keyed or derive_key hasher takes file input through `update_file()`.
Authenticated file manifests at full pipeline speed are one line either
way:

```cpp
auto tag = blake3pp::hash_file(path, pool.get_scheduler(), {.key = key});

blake3pp::hasher mac = blake3pp::hasher::keyed(key);   // same thing, spelled out
blake3pp::update_file(mac, path, pool.get_scheduler());
auto tag2 = mac.finalize();
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
has three implementation strategies, and which is fastest is not
predictable from the CPU. It depends on whether the AVX-512 datapath is
full-width or double-pumped (no CPUID bit reports that) and, on the
same machine, on whether the input fits in cache. Measured:

| machine | input | fastest | margin |
|---------|-------|---------|--------|
| Skylake-SP, Strix Point | streaming | `quartered` | 13-17% |
| Ryzen AI Max 395 | 8 MiB (in cache) | `quartered` | 18% over `staging` |
| Ryzen AI Max 395 | 512 MiB (streaming) | `staging` | 8% over `quartered` |

Two AMD parts with identical feature flags, opposite rankings. The whole
spread is about 10%, so the default is never a disaster, but there is no
setting that is right everywhere. Pick your cost:

```cpp
// 1. Do nothing. The default (quartered) won on most parts measured.

// 2. Tune at startup for the size you actually hash; the answer depends
//    on it. One streaming pass per strategy; allocates that buffer.
blake3pp::transpose16 best = blake3pp::tune_transpose16(512u << 20);

// 3. Tune once ever: persist the name, restore it on later starts.
save(std::string{blake3pp::to_string(best)});
blake3pp::set_transpose16(
    blake3pp::transpose16_from_string(load()).value_or(
        blake3pp::transpose16::quartered));
```

If you need to live at the edge, don't trust any of that: **measure your
own workload and set what works best for you**. `blake3pp_bench` times all
three (`t16-*` rows), and `blake3pp_bench --t16-sweep` ranks them across
input sizes so you can see where your workload sits; then pin the winner
with `set_transpose16()`. The tuner samples one size on an otherwise idle
machine, which is not the same thing as your program under load.

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
the sequential one. The easiest scheduler is the process-wide one, in
P2079's shape, the same call C++26 application code makes:

```cpp
#include <blake3pp/parallel.hpp>

auto sched = blake3pp::get_parallel_scheduler();
blake3pp::digest d = blake3pp::hash(big_buffer, sched);
```

Anyone who needs a sized or bounded pool constructs their provider's pool
directly and passes its scheduler instead (e.g. stdexec's
`exec::static_thread_pool pool(8); ... hash(big_buffer, pool.get_scheduler())`).

The sender/receiver provider itself is a build-time choice
(`-DBLAKE3PP_EXECUTION_PROVIDER=auto|std|beman|stdexec`): `std::execution`
where the standard library ships it, [beman.execution]
(conformance-first, C++23+) or NVIDIA stdexec (the default polyfill,
C++20+) otherwise. `blake3pp::execution_provider()` reports which one a
binary carries.

[beman.execution]: https://github.com/bemanproject/execution

When the data arrives in pieces, `parallel_hasher` has the exact
interface of `hasher`, with the multi-core fan-out (and all of BLAKE3's
subtree-alignment and final-chunk discipline) handled internally:

```cpp
blake3pp::parallel_hasher ph{blake3pp::get_parallel_scheduler()};
while (auto block = source.next_block()) {
  ph.update(*block);
}
blake3pp::digest streamed = ph.finalize();   // == the sequential digest

// Tunable, and checkpointable mid-stream just like hasher:
blake3pp::parallel_hasher tuned{blake3pp::get_parallel_scheduler(),
                                {.window_bytes = 16 * 1024 * 1024}};
```

### Hashing files at storage speed

The file pipeline streams a file through fixed windows using the fastest
OS-native mechanism (io_uring + `O_DIRECT` on Linux, IOCP +
`FILE_FLAG_NO_BUFFERING` on Windows, GCD/libdispatch + `F_NOCACHE` on
macOS), bypassing the page cache and overlapping reads with hashing; it
degrades gracefully per feature
(no direct I/O -> buffered async -> plain synchronous reads -> stdio). Paths are
`std::filesystem::path`; every entry point has a throwing form and a
`std::error_code` form, mirroring the standard library.

`hash_file()` is the one-shot form. `update_file()` is the primitive
underneath it: `hasher::update()` with a file as the source. It streams
into a hasher you own and returns, so the hasher's mode and every
finalize form apply to file input, and files hash in sequence:

```cpp
#include <blake3pp/io.hpp>   // sequential; standard library only

auto d = blake3pp::hash_file("dataset.parquet");        // throws system_error

std::error_code ec;
auto d2 = blake3pp::hash_file(config.input_path, ec);   // reports via ec
if (ec) { log_error(ec.message()); }

// A derived key from a file's bytes, as a seekable stream:
blake3pp::hasher h = blake3pp::hasher::derive_key("fixture v3 2026-09");
blake3pp::update_file(h, "seed.bin");
auto stream = h.finalize_xof();

// The hash of several files as one message:
blake3pp::hasher all;
for (const auto& part : parts) { blake3pp::update_file(all, part); }
auto d4 = all.finalize();
```

Adding cores means adding a scheduler, and that is the one thing that
pulls in an execution provider, so it lives in its own header. Both entry
points take one; the pipeline's knobs are `file_io_options`, and
`hash_file_options` (which adds the SIMD variant and an optional key for
the hasher it builds) converts to them, so one options object can drive
both:

```cpp
#include <blake3pp/parallel_io.hpp>   // io.hpp + parallel.hpp

// Full pipeline: async reads + multi-core hashing, tuned:
auto d3 = blake3pp::hash_file(path, pool.get_scheduler(),
                              {.window_bytes = 16 * 1024 * 1024,
                               .queue_depth  = 8});

blake3pp::update_file(h, path, pool.get_scheduler(),
                      {.window_bytes = 16 * 1024 * 1024});
```

The parallel form hands each complete window to the scheduler as a
subtree, which needs the hasher to sit on a window-aligned boundary when
the file starts: true for a fresh hasher and after files whose sizes are
window multiples, and otherwise the windows go through `update()`
instead. The digest is the same either way; only the parallelism varies.

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
built on extended output: the same seed (`--seed`, or the bytes of a
`--seed-file` streamed through the file pipeline) always yields the same
infinite stream, and `--seek` is O(1), so materializing a slice at
offset 10 GB costs the same as offset 0. Generation runs lanes-parallel
in the kernel (~3.8 GiB/s per core) and `--threads` fans segments across
cores via the O(1) seek (13+ GiB/s), so the sink is the bottleneck;
`--output` removes even that overhead, writing through io_uring +
O_DIRECT on Linux, IOCP + no-buffering on Windows or GCD + F_NOCACHE on
macOS with the stream generated straight into the write buffers,
bypassing the page cache entirely:

```bash
blake3ppgen --seed run42 --length 1G > testdata.bin
blake3ppgen --seed run42 --seek 10G --length 1M > slice.bin   # instant
blake3ppgen --seed run42 --length 100G --threads 0 \
            --output fixture.bin                              # device-bound
```

### Consuming via CMake

Vendoring is the normal path, and the one that probes *your* toolchain:

```cmake
include(FetchContent)
FetchContent_Declare(blake3pp GIT_REPOSITORY <this-repo> GIT_TAG main)
FetchContent_MakeAvailable(blake3pp)
target_link_libraries(app PRIVATE blake3pp::blake3pp)
```

The library is C++20; with a C++26 toolchain it uses native `std::simd`,
otherwise `std::experimental::simd` or xsimd: the same source, probed at
configure time. Tests, benchmarks and the `blake3ppsum` CLI only build
when blake3pp is the top-level project, so vendoring costs you nothing
beyond the library itself.

### Installing and find_package

```bash
cmake --preset linux-gcc16-cxx26
cmake --build --preset linux-gcc16-cxx26
cmake --install build/linux-gcc16-cxx26 --prefix /opt/blake3pp
```

```cmake
find_package(blake3pp REQUIRED)          # CMAKE_PREFIX_PATH=/opt/blake3pp
target_link_libraries(app PRIVATE blake3pp::blake3pp)
```

What gets installed is the static library, the public headers, and the
package config, nothing else. The archive is **self-contained**: the
per-architecture SIMD kernels are compiled into it, and it carries no link
dependency on any third-party library, so there is no `find_dependency()`
in the config and nothing to install alongside it. `BLAKE3PP_INSTALL`
(default: on for top-level builds) turns the rules off for consumers who
vendor.

One thing does not travel, and it is the reason for the last column of the
header table above. The execution provider is header-only and belongs to
whoever builds the final program, so blake3pp does not install it. Code
that includes `<blake3pp/parallel.hpp>` or `<blake3pp/parallel_io.hpp>`
(including anything that pulls the umbrella header) must put that
provider's headers on its own include path:

```cmake
find_package(blake3pp REQUIRED)
message(STATUS "built against: ${blake3pp_EXECUTION_PROVIDER}")   # e.g. stdexec
target_include_directories(app SYSTEM PRIVATE ${STDEXEC_INCLUDE_DIR})
```

The imported target defines the matching `BLAKE3PP_EXECUTION_*` macro, so
those headers select the same provider branch the library was compiled
against, and `blake3pp_EXECUTION_PROVIDER` tells you which one that was.
Consumers that stay on `core.hpp` / `dispatch.hpp` / `io.hpp` need none of
this: link the target and go.

Because the choices are frozen at install time, an installed package is
only valid for toolchains ABI-compatible with the one that built it; the
package version file declares `SameMajorVersion` compatibility, which
covers blake3pp's own API but says nothing about your compiler. If in
doubt, vendor.

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
two `-asan` variants. On a Mac, `macos-appleclang-cxx23` is the native
preset (Apple clang, GCD + F_NOCACHE I/O) and `macos-clang22-cxx26`
builds with Homebrew LLVM 22 against its bundled libc++ (`brew install
llvm`). Without a preset, a bare `cmake -S . -B build` configures the
C++20 baseline with the default compiler.

The devcontainer carries only the default gcc/clang pair; every other preset
runs inside its per-compiler toolchain image via `tools/tc <preset>`;
see `docker/README.md` for the image matrix and its glibc-floor design.

### Kernel tuning switches

Three cache variables turn measured kernel optimizations on or off. Each
is `auto|on|off` and defaults to `auto`, which is the fastest setting on
every machine it has been measured on. **These are not performance
options to tune, they are measurement controls**, and turning one off
makes the library slower. They exist so a result can be re-checked on
hardware its original measurement did not cover; the resolved value is
reported at configure time when it is not the default.

| variable | default | off means |
|----------|---------|-----------|
| `BLAKE3PP_KERNEL_INLINE_ENFORCEMENT` | on, all targets | Drop `always_inline`/`__forceinline` from the round core. Every compiler measured then outlines it (clang the whole `all_rounds`, GCC the `index_sequence` lambda), costing 6-40% depending on compiler and variant. |
| `BLAKE3PP_KERNEL_SRI_ROTATE` | on, aarch64 | Spell rot12/rot7 as the generic shift-or, which selects `shl`+`usra` instead of `shl`+`sri`. Worth ~6% on Apple M2 / clang 22; unverified on Neoverse. |
| `BLAKE3PP_KERNEL_STAGED_ROUNDS` | on, aarch64 | Run each round as sequential `g` calls instead of quartet-staged. A small win on Apple M2 / clang 22, and provably inert on GCC 15 (same schedule, different register names). Loses on x86, where it is off regardless. |

```bash
# Re-run the inlining A/B on a machine this project has never measured:
cmake --preset macos-clang22-cxx26 -DBLAKE3PP_KERNEL_INLINE_ENFORCEMENT=off
```

`BLAKE3PP_KERNEL_EXTRA_FLAGS` (a semicolon-separated list) appends raw
compiler flags to the kernel TUs only, for one-off flag trials that have
not earned a switch.

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
