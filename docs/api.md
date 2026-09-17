# The API

[← blake3pp](../README.md)

Everything the library exposes, with the examples that go with it.
Everything lives in `namespace blake3pp`, and
`#include <blake3pp/blake3pp.hpp>` brings in all of it; the granular
headers are in [Integration](integration.md#header-layout).

This is the guided tour. For every declaration exactly as the headers
state it, see the [reference](reference/index.md), which is generated
from their doc comments.

| | |
|---|---|
| [One-shot hashing](#one-shot-hashing) | `hash`, `digest`, hex, verification |
| [Incremental hashing](#incremental-hashing) | `hasher`, checkpoints, `reset` |
| [Keyed hashing and key derivation](#keyed-hashing-and-key-derivation) | `keyed_hash`, `derive_key` |
| [Extended output (XOF)](#extended-output-xof) | `finalize<N>`, `output_reader`, seeking |
| [Hashing files](#hashing-files-at-storage-speed) | `hash_file`, `update_file`, the pipeline knobs |
| [Multi-core hashing](#multi-core-hashing) | the same calls over a scheduler |
| [SIMD variants](#simd-variants-introspection-and-pinning) | what a build carries, what it picked, pinning |
| [Expert seams](#expert-seams) | bringing your own kernel table |

## One-shot hashing

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

## Incremental hashing

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

## Keyed hashing and key derivation

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

Both modes compose with everything else: `hasher::keyed` and
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

## Extended output (XOF)

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
scheduler (`<blake3pp/parallel.hpp>`),
`blake3pp::fill(r, big_buffer, sched)` splits the request into segments
that fill on every core, each
straight into its slice of the buffer, and leaves `r` positioned exactly
as `blake3pp::fill(r, big_buffer)` (the free spelling of `r.fill()`)
would have. That is what `blake3ppgen --threads` runs on.

Extended output works in all three modes (plain, keyed, derive_key): a
keyed hasher's `finalize_xof()` streams the MAC'd output, and
`derive_key` hashers can emit subkeys of any width.

## Hashing files at storage speed

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

On Linux each read is issued on io_uring's worker threads
(`IOSQE_ASYNC`), not inline in the submitting call. Issuing a large
direct read is real CPU work (pinning the pages, splitting and queueing
the bios: 1.5-1.9 ms per 64 MiB on a four-drive PCIe 5 stripe under
kernel 7.0), and inline it lands on the thread that also waits for the
window's hash; kernels since 6.x take the inline path whenever they
can, which halved the pipeline there (22 against 41 GiB/s). The
`offload_submit` option turns the hand-off off for callers who want the
submit inline (`--inline-submit` in the tools); the writer behind
`blake3ppgen --output` takes the same option.

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

## Multi-core hashing

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

`get_parallel_scheduler()` (P2079) returns one scheduler per process and
takes no size: under the beman provider it runs on every core, whatever
thread count a caller asks for. A program chooses otherwise by replacing
the backend behind it, one definition per program.
`-DBLAKE3PP_SIZED_PARALLEL_SCHEDULER=ON` builds that replacement as a
separate target:

```cpp
#include <blake3pp/parallel_backend.hpp>   // not in the umbrella header

blake3pp::size_parallel_scheduler(8);      // once, before first use
auto d = blake3pp::hash(buf, blake3pp::get_parallel_scheduler());
```

The definition lives in `blake3pp::parallel_backend`, which an executable
links when it wants one; `<blake3pp/parallel_backend.hpp>` is not part of
the umbrella header. Under the beman provider the option also builds
beman.execution with its own default backend disabled, leaving a single
definition of `query_parallel_scheduler_backend()` in the program. The
scheduler-taking overloads accept any scheduler and need none of this.

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

## SIMD variants: introspection and pinning

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

## Expert seams

For engines that compute subtrees externally (custom pipelines, the I/O
layer itself), `hasher::push_subtree_cv()` absorbs an
externally-computed subtree chaining value, and `hasher` / the parallel
`hash()` accept a caller-supplied kernel table; that seam is how the
benchmark plugs upstream's hand-written assembly into this pipeline.
