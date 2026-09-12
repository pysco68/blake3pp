# blake3pp

A C++20-and-later BLAKE3 implementation built as a case study in
hardware-saturating, portable C++26-forward design: `std::simd` and
`std::execution` where the standard library provides them, drop-in
polyfills (xsimd, stdexec) where it doesn't, OS-native direct I/O behind
a unified interface, and multi-architecture SIMD kernels compiled into a
single binary with zero-overhead runtime dispatch:

| architecture  | kernels |
|---------------|---------|
| x86-64        | SSE4.2, AVX2, AVX-512 |
| ARM (aarch64) | NEON; fixed-length SVE 256/512 and SVE2 128 (more VLs opt-in) |
| RISC-V        | RVV 1.0 at VLEN 128/256/512, each with a Zvbb-rotate twin; opt-in T-Head draft-0.7.1 XTheadVector |
| POWER         | VSX |
| IBM z         | VXE (z14+, **big-endian**) |
| MIPS          | MSA (MIPS32r5/MIPS64r5+) — **emulator-tested only**, see below |
| wasm          | SIMD128 (see [The wasm build](#the-wasm-build)) |

Every build also carries a scalar kernel as fallback option. 

The MSA kernel carries an asterisk the others do not. It is validated
against the official test vectors under qemu, byte-identical to the
x86-64 result, and its dispatch is validated both ways: an emulated core
without MSA falls back to scalar, one with it selects the kernel. No MSA
silicon has ever run it, and none is reachable, so it ships with no
throughput number and should be treated as untested on hardware. It is
built by GCC through the vector-extension provider, because no std
provider deduces a vector width on this target and xsimd has no MSA
backend, and clang emits no MSA from the same source at all. That is
why the MIPS archive links a static glibc where the others link a
static musl: it is equally standalone, with no interpreter, no dynamic
section and no `GLIBC_` version symbol, so it carries no glibc floor.
The one thing a static glibc gives up is NSS, which this tool never
asks for.

The SVE kernels are vector-length-specific: dispatch selects one only
when the CPU's runtime vector length equals the length the kernel was
compiled for, since that is the only case the ABI guarantees. That
length comes from `prctl` on Linux and from `rdvl` on Windows, which
reports whether SVE is present but not how wide it is. Compiling them
needs a compiler that accepts a fixed vector length: `clang-cl` does, in
its cc1 spelling, and `cl` does not, so an arm64 Windows build carries
SVE kernels only when clang-cl builds it.

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

CI publishes binary releases as dev-* prereleases from every green main
run:

| release | targets | toolchain |
|---------|---------|-----------|
| Linux, fully static | x86_64, aarch64, riscv64, ppc64le, s390x | zig (clang + musl) |
| Windows | x64 and arm64 | clang-cl from the VS toolset |
| macOS | Apple silicon | Apple clang |
| wasm | wasm32-simd128, node-runnable js+wasm pairs | Emscripten |

The static Linux binaries are one file per architecture that runs on
any distro: no glibc version coupling, no dynamic loader, runtime SIMD
dispatch intact. Two of them are even the work of two compilers at
once. How that works, and why it holds up, is explained in
[docs/static-linux-binaries.md](docs/static-linux-binaries.md).

Everything lives in `namespace blake3pp`, and
`#include <blake3pp/blake3pp.hpp>` gets you all of it. Compile-cost-aware
consumers can pick granular headers instead:

| header                       | provides | needs an execution provider |
|------------------------------|----------|:---:|
| `<blake3pp/blake3pp.hpp>`    | umbrella: everything below | yes |
| `<blake3pp/dispatch.hpp>`    | `arch` introspection, SIMD variant selection | no |
| `<blake3pp/core.hpp>`        | `digest`, `hasher`, one-shot `hash()` | no |
| `<blake3pp/io.hpp>`          | `update_file()` and `hash_file()`, the async direct-I/O pipeline | no |
| `<blake3pp/parallel.hpp>`    | multi-core `hash()`, `parallel_hasher`, multi-core XOF `fill()` | yes |
| `<blake3pp/parallel_io.hpp>` | `update_file()` and `hash_file()` over a scheduler (the two combined) | yes |

The last column is the one that matters when you install blake3pp rather
than build it: only the scheduler-taking headers include an execution
library (stdexec, beman.execution, or `<execution>`). Hashing buffers and
hashing files sequentially compile against the standard library alone.

The split follows one rule. Members are the sequential primitives of the
value types (`update`, `finalize`, `finalize_xof`, `fill`, `take`,
`seek`). Free functions are the entry points that bring in a resource
the type does not own, a scheduler or a file, and each comes as a pair
so call sites read alike with and without cores: `hash(data)` /
`hash(data, sched)`, `update_file(h, path)` / `update_file(h, path, sched)`,
 `fill(r, out)` / `fill(r, out, sched)`. The scheduler-taking
half of each pair lives in the header that owns the dependency.
The two I/O headers exist only when the library is built with
`BLAKE3PP_WITH_IO=ON` (the default); see
[Freestanding and RTOS builds](#freestanding-and-rtos-builds).

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

// Any byte sequence, any length (extended output, keys): by value or
// into a caller's buffer.
std::string wide_hex = blake3pp::to_hex(h.finalize<64>());
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
`hasher::derive_key` give incremental hashing, `keyed_hash(key, data, sched)` 
and the keyed and derive_key `parallel_hasher` constructors go multi-core, 
and a keyed or derive_key hasher takes file input through `update_file()`. 
Authenticated file manifests at full pipeline speed are one line either way:

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

auto wide_key = h.finalize<64>();           // std::array<std::byte, 64>

std::vector<std::byte> runtime_sized(n);
h.finalize(runtime_sized);                  // any length, caller's buffer

blake3pp::output_reader r = h.finalize_xof();
r.fill(first_chunk);                        // stream sequentially...
auto next = r.take<32>();                   // ...same, by value
r.seek(10'000'000'000);                     // ...or jump: O(1) random access
r.fill(deep_chunk);                         // byte 10 GB costs same as byte 0
```

O(1) seek also makes the stream embarrassingly parallel: with a
scheduler (`<blake3pp/parallel.hpp>`), `blake3pp::fill(r, big_buffer,
sched)` splits the request into segments that fill on every core, each
straight into its slice of the buffer, and leaves `r` positioned exactly
as `blake3pp::fill(r, big_buffer)` (the free spelling of `r.fill()`)
would have. That is what `blake3ppgen --threads` runs on.

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

On machines with a width-16 kernel (AVX-512; SVE-512 on ARM; RVV at
VLEN=512) one more dial exists: the 16-lane message transpose has three implementation
strategies, and which is fastest is not predictable from the CPU. It
depends on whether the vector datapath is full-width or double-pumped
(no CPUID bit reports that) and, on the same machine, on whether the
input fits in cache. Measured (all AVX-512 parts so far):

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

### The wasm build

The wasm32-simd128 build is the one target where the fat-binary premise
cannot hold, because of how WebAssembly validates modules. On native
ISAs an unsupported instruction is harmless as long as it never
executes; that is exactly what lets one binary carry AVX-512 next to
SSE4.2 and choose at runtime. A wasm engine instead validates the
entire module at load time: if the engine does not support SIMD128, a
module containing any SIMD opcode is rejected before a single
instruction runs, whether those opcodes would ever have executed or
not.

So there is nothing to select between inside one module. Code that is
running has by definition already passed validation, so SIMD128 is
simply available (`available_arches()` reports it unconditionally
there). The scalar kernel is still compiled in and can be pinned for
comparison runs, but it is not a fallback: an engine without SIMD128
never gets far enough to reach it. Graceful degradation on wasm means
shipping two modules and picking one at load time on the embedder side
(feeding `WebAssembly.validate()` a small SIMD probe module is the
standard test). Every current mainstream engine supports SIMD128
(node 16+ has it on by default), which is why the release ships the
simd128 build only.

### Multi-core hashing (std::execution / stdexec)

Every entry point is available sequentially or multi-core; the concepts
are orthogonal:

|             | one-shot           | incremental        | file input               | extended output       |
|-------------|--------------------|--------------------|--------------------------|-----------------------|
| sequential  | `hash(data)`       | `hasher`           | `update_file(h, path)`   | `fill(r, out)`        |
| multi-core  | `hash(data, sched)`| `parallel_hasher`  | `update_file(h, path, sched)` | `fill(r, out, sched)` |

The one-shot column has keyed and derive_key siblings (`keyed_hash`,
`derive_key`) in both rows, each also taking a `std::string_view`.

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

The input is split into parts that the scheduler's agents pull as they
go, and each part's 32-byte chaining value waits on the calling thread's
stack. A `stack_budget` template argument sets how much stack that table
may take, 32 KiB (1024 parts) by default. Where stacks are small, give it
less and the input is split into fewer, larger parts; a budget that is not
a multiple of 32 bytes, or holds fewer than two parts, does not compile:

```cpp
blake3pp::digest d = blake3pp::hash<blake3pp::stack_budget{1024}>(buffer, sched);   // 32 parts
```

Code that knows its calling thread's stack can check the budget against it
at compile time:

```cpp
constexpr blake3pp::stack_budget budget{1024};
static_assert(budget.bytes <= CONFIG_MAIN_STACK_SIZE / 4);   // e.g. on Zephyr
```

The budget covers that table and nothing else. Each part is reduced on the
agent that took it, by a recursion that holds one chaining-value buffer per
level: 2 KiB per level while the build contains a 16-wide kernel, since the
buffer is sized for the widest variant compiled rather than the one that
runs, and as many levels as halving the part takes to reach twice the
running variant's degree in chunks. Agent threads therefore need stack of
their own, and the two costs pull against each other: a smaller budget
splits the input into larger parts, which makes that recursion deeper.
Sizing a thread from the budget alone is not enough.

`keyed_hash`, `derive_key`, `update_file` and `hash_file` take the same
argument, and `parallel_hasher` takes it as its second template parameter.

The sender/receiver provider itself is a build-time choice
(`-DBLAKE3PP_EXECUTION_PROVIDER=auto|std|beman|stdexec`), and
`blake3pp::execution_provider()` reports which one a binary carries:

| provider | floor | role |
|----------|-------|------|
| `std::execution` | a standard library that ships it | preferred when available |
| [beman.execution] | C++23 | conformance-first polyfill |
| NVIDIA stdexec | C++20 | default polyfill |

[beman.execution]: https://github.com/bemanproject/execution

When the data arrives in pieces, `parallel_hasher` has the exact
interface of `hasher` (all three modes, the whole finalize family
including `finalize_xof()`), with the multi-core fan-out and all of
BLAKE3's subtree-alignment and final-chunk discipline handled
internally:

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
OS-native mechanism, bypassing the page cache and overlapping reads with
hashing:

| OS | async engine | page-cache bypass |
|----|--------------|-------------------|
| Linux | io_uring | `O_DIRECT` |
| Windows | IOCP | `FILE_FLAG_NO_BUFFERING` |
| macOS | GCD (libdispatch) | `F_NOCACHE` |

Each feature degrades independently at runtime: no direct I/O ->
buffered async -> plain synchronous reads -> stdio. Paths are
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
spec:

- `--keyed FILE`: MAC mode; the key is 32 raw bytes or 64 hex chars,
  read from a file so it never appears on the command line
- `--derive-key CONTEXT`: domain-separated KDF mode
- `--length N`: extended (XOF) output
- `--check`: verify previously printed checksum lines
- `--arch`, `--threads` (default: all cores; 1 = sequential), and the
  I/O pipeline knobs
- `--version`: the providers and SIMD variants baked into the binary

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
in the kernel and `--threads` fans segments across cores via the O(1) seek, 
so the sink is the bottleneck;
`--output` removes even that overhead, writing through io_uring +
O_DIRECT on Linux, IOCP + no-buffering on Windows or GCD + F_NOCACHE on
macOS with the stream generated straight into the write buffers,
bypassing the page cache entirely:

```bash
blake3ppgen --seed run42 --length 1G > testdata.bin
blake3ppgen --seed run42 --seek 10G --length 1M > slice.bin   # instant
blake3ppgen --seed run42 --length 100G --output fixture.bin   # device-bound
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

### Freestanding and RTOS builds

The library core runs on targets with no filesystem and no OS threads;
what drove this was a Zephyr SMP port running blake3pp on the RP2350
in both of its personalities (Cortex-M33 and Hazard3 RISC-V). Two
build-time switches make it fit:

- `-DBLAKE3PP_WITH_IO=OFF` drops the file-I/O layer entirely. It needs
  a filesystem, 64-bit seeks and OS-specific async I/O, none of which
  a microcontroller RTOS has; without it the library is `hasher`,
  `digest`, dispatch and the scheduler-taking parallel API. The
  umbrella header follows the option, and tests, tools and the file
  bench gate themselves on it (the in-memory bench and the non-I/O
  test suite still build).
- Toolchains without OS threads are detected, not fought: where
  `<thread>`/`<mutex>` are empty (the Zephyr SDK's libstdc++ is built
  without gthreads), the `BLAKE3PP_HAS_STD_THREAD` probe comes back
  negative and `get_parallel_scheduler()` simply does not exist.
  Nothing else is lost: the scheduler-taking `hash()` overloads are
  the primary API anyway, and a freestanding caller brings its own
  scheduler because only it knows what its execution agents should be
  (on Zephyr SMP, for instance, one per core).
- The multi-core entry points keep one 32-byte chaining value per part
  on the calling thread's stack, 32 KiB by default, more than a
  microcontroller thread usually has. A `stack_budget` template argument
  sizes that table to the thread, and a `static_assert` of the budget
  against the RTOS's own stack size catches one that does not fit (see
  the multi-core section above). The budget bounds that table only. The
  threads behind the scheduler pay the subtree recursion described
  there, some 2 KiB per level, so they need provisioning as well, and a
  smaller budget deepens the recursion on them as it shrinks the table
  on the caller. Those per-level buffers are sized for the widest kernel
  the build could contain, so a scalar-only target cuts them from 2 KiB
  to 128 bytes with `-DBLAKE3PP_MAX_SIMD_DEGREE=1`; a kernel wider than
  the value fails the build rather than overflowing them.

Everything else adapts by the existing probes: 32-bit targets are
supported, and the SIMD/execution polyfills select exactly as on
hosted platforms.

### Guarantees and caveats

- The sequential compute paths never allocate: `hasher` is a flat value
  type, subtree buffers live on the stack, and the I/O pipeline
  allocates only its buffer ring at setup. The parallel engine is the
  exception: `parallel_hasher` allocates its accumulation window at
  construction, and each bulk launch allocates whatever the execution
  provider needs (stdexec: one small task array per launch).
- A `hasher` instance is not thread-safe; distinct instances and all free
  functions are.
- The library never mutates process-global state as a side effect:
  default CPU detection is auxv/hwprobe/CPUID reads only. The one
  detection step that needs more (RISC-V vendor-kernel shapes, where
  the classifier is an instruction probed under a scoped SIGILL guard)
  is opt-in via `blake3pp::run_trap_probes()`; without the call, those
  machines conservatively report scalar. The CLI tools opt in at
  startup, since a standalone binary owns its process.
- The sequential compute API (`core.hpp`, `dispatch.hpp`) is `noexcept`
  end to end, with one exception: `digest::to_hex()` builds a
  `std::string` (use `to_hex_chars()` for the allocation-free,
  nonthrowing form). The I/O layer throws `std::system_error`, or
  reports through the `std::error_code` overloads, which also fold
  allocation failure into an error code. The scheduler-taking parallel
  APIs propagate whatever the execution provider raises, plus
  `bad_alloc`.
- Full BLAKE3 spec surface: plain, keyed and derive_key modes, each with
  arbitrary-length (XOF) output; every mode is verified against all 131
  output bytes of the official test vectors.

## Building

```bash
cmake --preset linux-gcc16-cxx26 && cmake --build --preset linux-gcc16-cxx26
ctest --preset linux-gcc16-cxx26
```

Any name from `cmake/toolchains/` works as a preset (see
`CMakePresets-toolchains.json`). Test presets exist for:

- `linux-gcc16-cxx26` and `linux-clang22-cxx26`
- `linux-clang18-cxx20-libstdcxx`: the C++20 polyfill path
- the two `-asan` variants

On a Mac:

- `macos-appleclang-cxx23`: the native preset (Apple clang, GCD +
  F_NOCACHE I/O)
- `macos-clang22-cxx26`: Homebrew LLVM 22 against its bundled libc++
  (`brew install llvm`)

Without a preset, a bare `cmake -S . -B build` configures the C++20
baseline with the default compiler.

The devcontainer carries only the default gcc/clang pair; every other preset
runs inside its per-compiler toolchain image via `tools/tc <preset>`;
see `docker/README.md` for the image matrix and its glibc-floor design.
The cross presets (`linux-arm64-gcc15-cxx23`, `linux-riscv64-gcc15-cxx23`,
and the static musl trio `linux-{,arm64-,riscv64-}zigmusl-cxx23-static`)
run their whole test suites under qemu-user with a selectable vector
length, so one build exercises every SVE VL or RVV VLEN; the emulator
recipes are in `docker/README.md` too. Two kernel sets are opt-in:

- `-DBLAKE3PP_SVE_ALL_VARIANTS=ON` adds the SVE variants matching no
  shipping silicon (emulator targets)
- `-DBLAKE3PP_XTHEAD_KERNEL=ON` compiles the hand-written T-Head
  XTheadVector (draft RVV 0.7.1) kernel, which only T-Head's qemu fork
  can execute

### Building through cmake-re

Every containerized preset also builds through [tipi cmake-re], which
runs the compile actions on a remote-execution cluster (EngFlow, through
reclient) and serves repeats from its action cache; the tests, emulator
matrices and artifacts are the same as with plain cmake. cmake-re ships
in every x86_64 toolchain image (`TIPI_DISTRO_MODE=none`, so it drives
the image's own compilers). It knows nothing about presets, so
`tools/preset-args.py` unrolls one into plain configure arguments:

```bash
tools/tc linux-gcc16-cxx26 -- bash -c '
  cmake-re --host -S . $(python3 tools/preset-args.py linux-gcc16-cxx26)
  cmake-re --build build/linux-gcc16-cxx26 --host -j"$(nproc)"
  ctest --test-dir build/linux-gcc16-cxx26 --output-on-failure'
```

`--host` builds inside the image the command runs in; add
`--distributed` to send the compiles to the cluster, with the mTLS
credentials in `RBE_service`, `RBE_tls_client_auth_key` and
`RBE_tls_client_auth_cert`, and `TIPI_DISABLE_AR_RANLIB_DRIVER=ON` and
`USER` set. The build directory becomes a symlink into cmake-re's mirror
of the checkout under `.tipi`, which is why `ctest --test-dir` is the
spelling above.

Each toolchain lives in its own folder under `cmake/toolchains/`,
together with the `.pkr.js` and `.layers.json` that name its image at
the content tag and the manifest digest that tag resolves to: that
folder is the environment cmake-re copies when it is not told `--host`,
and the cluster pulls the same image for the compile actions.
`tools/gen-environments.py` writes these files (`--check` reports stale
ones; the digests come from the registry or from a `--digests` map),
and `BLAKE3PP_TC_REGISTRY` with `BLAKE3PP_TC_IMAGE_TEMPLATE` retarget
them at a registry mirror the cluster can reach. CI does not depend on
them being current: every run writes the tag and digests it builds with
into them and builds on that, and when the committed files lag, a run
of `main` opens one draft pull request (`ci/toolchain-environments`,
updated in place while the tag keeps moving) proposing the update, and
marking it ready for review runs CI on it; a release built on injected
files is named and annotated as a dirty build.

In CI the repository variable `BLAKE3PP_CI_DRIVER=cmake-re` (or the
`driver` input of a manual run) switches every Linux container lane to
cmake-re, building with `--host` alone (the image's own compilers, no
cluster involved) unless `BLAKE3PP_CMAKE_RE_MODE` (or the
`cmake-re-mode` input) says `distributed`. The settings behind it:

| Setting | Kind | Purpose |
| --- | --- | --- |
| `BLAKE3PP_CMAKE_RE_MODE` | variable | `host` (default) or `distributed`; everything below matters only for `distributed` |
| `RBE_SERVICE` | secret | cluster address |
| `RBE_TLS_CLIENT_AUTH_KEY`, `RBE_TLS_CLIENT_AUTH_CERT` | secrets | the mTLS client credentials, PEM |
| `BLAKE3PP_RBE_ENV_REGISTRY` | variable | the public mirror the cluster pulls the environment images from: a Docker Hub namespace, spelled without `docker.io/` (cmake-re matches the daemon's digests, which never carry that host); `toolchains.yml` copies every image it builds there |
| `DOCKERHUB_USERNAME`, `DOCKERHUB_TOKEN` | secrets | the mirror's credentials |

cmake-re's mirror of the checkout is restored from the actions cache
per lane, so a rerun configures in seconds and builds only what
changed. Windows and macOS stay native. `tools/make-release.sh` takes
the same switch (`BLAKE3PP_CMAKE_RE=1`, `BLAKE3PP_CMAKE_RE_FLAGS`
choosing `--host` or `--host --distributed`).

[tipi cmake-re]: https://tipi.build

### Kernel tuning switches

Five cache variables turn measured kernel optimizations on or off. Each
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
| `BLAKE3PP_KERNEL_XAR_ROTATE` | on, SVE2 variants | Spell `rot(x ^ y)` as `eor` + rotate instead of one fused `XAR`. Off costs the SVE2 kernels their entire margin over NEON: measured 1.89 vs 1.61 GiB/s on Neoverse V2 (GCP Axion). |
| `BLAKE3PP_KERNEL_VROR_ROTATE` | on, RVV Zvbb variants | Spell the rotate as the 4-op shift-or (base RVV has no rotate) instead of `vxor`+`vror`. The XAR playbook on RISC-V; unmeasured on real Zvbb silicon so far (this switch is how it will be). |

```bash
# Re-run the inlining A/B on a machine this project has never measured:
cmake --preset macos-clang22-cxx26 -DBLAKE3PP_KERNEL_INLINE_ENFORCEMENT=off
```

`BLAKE3PP_KERNEL_EXTRA_FLAGS` (a semicolon-separated list) appends raw
compiler flags to the kernel TUs only, for one-off flag trials that have
not earned a switch.

Layout:

- `include/blake3pp/`: the public API; the canonical variant list (enum,
  names, preference ranking) is generated from `detail/arch.def`
- `src/core/`: arch-agnostic tree logic
- `src/kernel/`: the per-architecture kernel, one TU compiled once per
  variant by `cmake/ArchKernels.cmake`; variants are registered per ISA
  family in `cmake/KernelVariants.cmake`
- `src/dispatch/`: runtime routing, with per-platform CPU probes in
  `cpu_detect_*.cpp`
- `cmake/StdFeatures.cmake`: probes what the active standard library
  really ships, by compiling usage rather than trusting feature-test
  macros
- `tests/`: verifies every configuration against the official BLAKE3
  test vectors

## License

Copyright 2026 Yannic Staudt. Apache License 2.0 with LLVM Exceptions; see
[LICENSE](LICENSE).
