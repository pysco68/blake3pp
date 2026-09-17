# The API

[← blake3pp](../README.md)

The surface is small because the choices are independent. What you
compute, how you feed it, what runs it and what it may spend are four
separate decisions, and picking one never constrains another. Everything
lives in `namespace blake3pp`, and `#include <blake3pp/blake3pp.hpp>`
brings in all of it; the granular headers are in
[Integration](integration.md#header-layout).

This page runs across those decisions. The [reference](reference/index.md) runs down each header.

## The choices, and how they compose

|             | one-shot           | incremental        | file input                    | extended output       |
|-------------|--------------------|--------------------|-------------------------------|-----------------------|
| sequential  | `hash(data)`       | `hasher`           | `update_file(h, path)`        | `fill(r, out)`        |
| multi-core  | `hash(data, sched)`| `parallel_hasher`  | `update_file(h, path, sched)` | `fill(r, out, sched)` |

Every cell takes the same key modes, returns the same digests, and
accepts the same variant pinning. Adding cores adds a scheduler argument
and changes nothing else; the one-shot column has `keyed_hash` and
`derive_key` siblings in both rows, each also taking a `std::string_view`.

## What you compute

### Mode: plain, keyed, derive_key

BLAKE3 is one function with three keying modes, and every part of this
library offers all three.

```cpp
blake3pp::digest d   = blake3pp::hash("hello world");
blake3pp::digest mac = blake3pp::keyed_hash(key, message);
auto session_key     = blake3pp::derive_key("example.com 2026-08 tls session",
                                            master_secret);
```

Keyed mode is BLAKE3's built-in MAC and PRF, the modern replacement for
HMAC. Keys are exactly 32 bytes. The span extent in the signatures makes a
wrong-sized key a compile error, not a runtime failure.

`derive_key` is the domain-separated KDF. The context string is not a
secret; it is what keeps unrelated uses of the same key material
cryptographically independent, so hardcode it and make it unique to the
application and purpose.

The mode is a property of the hasher, not of the call, which is why it
composes with everything else:

```cpp
blake3pp::hasher h = blake3pp::hasher::keyed(key);   // incremental MAC
blake3pp::update_file(h, path);                      // ...over a file
auto tag = blake3pp::hash_file(path, sched, {.key = key});   // ...and cores
```

### Output: 32 bytes, or a stream

The 32-byte digest is the first 32 bytes of an unbounded stream. Ask for
any length, or take the reader:

```cpp
auto wide = h.finalize<64>();               // std::array<std::byte, 64>

std::vector<std::byte> runtime_sized(n);
h.finalize(runtime_sized);                  // any length, caller's buffer

blake3pp::output_reader r = h.finalize_xof();
r.fill(first_chunk);                        // stream sequentially...
auto next = r.take<32>();                   // ...same, by value
r.seek(10'000'000'000);                     // ...or jump: O(1) random access
r.fill(deep_chunk);                         // byte 10 GB costs what byte 0 costs
```

Seeking is O(1) because each 64-byte block of the stream is one
compression carrying its own counter, depending on nothing before it.
That independence is also what makes the stream parallel: with a
scheduler, `blake3pp::fill(r, big_buffer, sched)` splits one request into
segments that fill on every core, each straight into its slice of the
buffer, and leaves `r` positioned exactly where `blake3pp::fill(r,
big_buffer)` would have. That free spelling of `r.fill()` is what lets
the sequential and multi-core forms read alike. It is what `blake3ppgen --threads` runs on.

Extended output works in all three modes: a keyed hasher's
`finalize_xof()` streams MAC'd output, and `derive_key` hashers emit
subkeys of any width.

### The digest as a value

```cpp
std::cout << d.to_hex() << '\n';                      // 64 lowercase hex
std::cout << std::format("digest: {}\n", d);          // std::format-able

std::array<char, 65> hex = d.to_hex_chars();          // no allocation, NUL-terminated
std::string wide_hex = blake3pp::to_hex(h.finalize<64>());   // any byte sequence
```

`digest` is a regular value type: compare with `==`, round-trip through
hex, use it as a map key after hashing its bytes. Comparison is
constant-time in every form, matching the Rust reference, which is the
safe default for a value compared against untrusted input.

```cpp
if (blake3pp::hash(payload).matches(user_input)) { /* verified */ }

// Parsing explicitly? std::optional's heterogeneous == compares the
// contained value, and is false for nullopt; no dereference needed.
if (blake3pp::digest::from_hex(user_input) == blake3pp::hash(payload)) {
  /* verified */
}
```

Hex that does not parse counts as no match, not as an error.

## How you feed it

### One shot, or incrementally

```cpp
blake3pp::hasher h;
while (auto block = source.next_block()) {
  h.update(*block);                         // span<const byte>, or string_view
}
blake3pp::digest checkpoint = h.finalize(); // everything so far
h.update(trailer);
blake3pp::digest full = h.finalize();       // ...and of the whole stream
h.reset();                                  // reuse the instance
```

`finalize()` is non-destructive and `const`: taking a digest is a read of
the hasher's state, so a long stream can be checkpointed without giving
up the hasher. How the input is chunked never changes the result.

### From memory, or from a file

Files are a first-class input, not a read loop the caller writes. The
pipeline streams a file through fixed windows using the fastest mechanism
the platform offers, bypassing the page cache and overlapping reads with
hashing:

| OS | async engine | page-cache bypass |
|----|--------------|-------------------|
| Linux | io_uring | `O_DIRECT` |
| Windows | IOCP | `FILE_FLAG_NO_BUFFERING` |
| macOS | GCD (libdispatch) | `F_NOCACHE` |

Each feature degrades independently at runtime: no direct I/O falls back
to buffered async, then to plain synchronous reads, then to stdio.

```cpp
#include <blake3pp/io.hpp>   // sequential; standard library only

auto d = blake3pp::hash_file("dataset.parquet");        // throws system_error

std::error_code ec;
auto d2 = blake3pp::hash_file(config.input_path, ec);   // reports via ec
if (ec) { log_error(ec.message()); }
```

`hash_file()` is the one-shot form. `update_file()` is the primitive
underneath it: `hasher::update()` with a file as the source. It streams
into a hasher you own and returns, so the hasher's mode and every
finalize form apply to file input, and files hash in sequence:

```cpp
// A derived key from a file's bytes, as a seekable stream:
blake3pp::hasher h = blake3pp::hasher::derive_key("fixture v3 2026-09");
blake3pp::update_file(h, "seed.bin");
auto stream = h.finalize_xof();

// The hash of several files as one message:
blake3pp::hasher all;
for (const auto& part : parts) { blake3pp::update_file(all, part); }
auto d4 = all.finalize();
```

Paths are `std::filesystem::path`. Codebases on Boost.Filesystem work
transparently: any path-like type with a `native()` observer is accepted
structurally, so `blake3pp::hash_file(boost_path)` compiles without
blake3pp knowing Boost exists, and without lossy transcoding on Windows.

#### Inside the pipeline

The knobs are `file_io_options`, and `hash_file_options` — which adds the
SIMD variant and an optional key for the hasher it builds — converts to
them, so one options object can drive both:

```cpp
#include <blake3pp/parallel_io.hpp>   // io.hpp + parallel.hpp

auto d3 = blake3pp::hash_file(path, pool.get_scheduler(),
                              {.window_bytes = 16 * 1024 * 1024,
                               .queue_depth  = 8});

blake3pp::update_file(h, path, pool.get_scheduler(),
                      {.window_bytes = 16 * 1024 * 1024});
```

On Linux each read is issued on io_uring's worker threads
(`IOSQE_ASYNC`), not inline in the submitting call. Issuing a large
direct read is real CPU work — pinning the pages, splitting and queueing
the bios, 1.5-1.9 ms per 64 MiB on a four-drive PCIe 5 stripe under
kernel 7.0 — and inline it lands on the thread that also waits for the
window's hash. Kernels since 6.x take the inline path whenever they can,
which halved the pipeline there, 22 against 41 GiB/s. The
`offload_submit` option turns the hand-off off for callers who want the
submit inline (`--inline-submit` in the tools), and the writer behind
`blake3ppgen --output` takes the same option.

The parallel form hands each complete window to the scheduler as a
subtree, which needs the hasher to sit on a window-aligned boundary when
the file starts. That is true for a fresh hasher and after files whose
sizes are window multiples; otherwise the windows go through `update()`
instead. The digest is the same either way, and only the parallelism
varies.

## What runs it

### Sequential, or over a scheduler

For large in-memory buffers, hand `hash()` any sender/receiver scheduler.
BLAKE3's tree makes the decomposition exact, so the digest is identical
to the sequential one.

```cpp
#include <blake3pp/parallel.hpp>

auto sched = blake3pp::get_parallel_scheduler();
blake3pp::digest d = blake3pp::hash(big_buffer, sched);
```

`get_parallel_scheduler()` is P2079's shape, the same call C++26
application code makes: one scheduler per process, taking no size. Anyone
who needs a sized or bounded pool constructs their provider's pool and
passes its scheduler instead, for instance stdexec's
`exec::static_thread_pool pool(8); ... hash(big_buffer, pool.get_scheduler())`.

When the data arrives in pieces, `parallel_hasher` has the exact
interface of `hasher` — all three modes, the whole finalize family
including `finalize_xof()` — with the fan-out and all of BLAKE3's
subtree-alignment and final-chunk discipline handled internally:

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

The sender/receiver provider is a build-time choice
(`-DBLAKE3PP_EXECUTION_PROVIDER=auto|std|beman|stdexec`), and
`blake3pp::execution_provider()` reports which one a binary carries:

| provider | floor | role |
|----------|-------|------|
| `std::execution` | a standard library that ships it | preferred when available |
| [beman.execution] | C++23 | conformance-first polyfill |
| NVIDIA stdexec | C++20 | default polyfill |

[beman.execution]: https://github.com/bemanproject/execution

Under the beman provider `get_parallel_scheduler()` runs on every core,
whatever thread count a caller asks for. A program chooses otherwise by
replacing the backend behind it, one definition per program.
`-DBLAKE3PP_SIZED_PARALLEL_SCHEDULER=ON` builds that replacement as a
separate target:

```cpp
#include <blake3pp/parallel_backend.hpp>   // not in the umbrella header

blake3pp::size_parallel_scheduler(8);      // once, before first use
auto d = blake3pp::hash(buf, blake3pp::get_parallel_scheduler());
```

The definition lives in `blake3pp::parallel_backend`, which an executable
links when it wants one. Under the beman provider the option also builds
beman.execution with its own default backend disabled, leaving a single
definition of `query_parallel_scheduler_backend()` in the program. The
scheduler-taking overloads accept any scheduler and need none of this.

### Which kernel

The binary carries every variant your target platform supports, and
dispatch picks the best one at runtime. You can look, and you can
override:

```cpp
for (blake3pp::arch a : blake3pp::compiled_arches())   // in this binary
  std::cout << blake3pp::to_string(a) << ' ';
for (blake3pp::arch a : blake3pp::available_arches())  // usable on this CPU
  std::cout << blake3pp::to_string(a) << ' ';          // best-first

blake3pp::hasher pinned{blake3pp::arch::sse42};        // explicit variant
assert(blake3pp::available_arches().front() == blake3pp::best_available());
```

Requesting a variant the CPU cannot run is not an error: dispatch falls
back to the best available one, and `is_available()` answers the question
beforehand.

The build configuration is introspectable too, which is what a
diagnostics banner or a bug report needs:

```cpp
std::cout << std::format("blake3pp {} (simd: {}, execution: {})\n",
                         blake3pp::version(), blake3pp::simd_provider(),
                         blake3pp::execution_provider());
// e.g. "blake3pp 0.1.0 (simd: std::simd, execution: stdexec)"
```

#### One dial that is not predictable

On machines with a width-16 kernel (AVX-512; SVE-512 on ARM; RVV at
VLEN=512) the 16-lane message transpose has three implementation
strategies, and which is fastest cannot be read off the CPU. It depends
on whether the vector datapath is full-width or double-pumped, which no
CPUID bit reports, and on the same machine it depends on whether the
input fits in cache. Measured, across all AVX-512 parts so far:

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

If you need to live at the edge, trust none of that and measure your own
workload. `blake3pp_bench` times all three (`t16-*` rows), and
`blake3pp_bench --t16-sweep` ranks them across input sizes so you can see
where your workload sits; then pin the winner with `set_transpose16()`.
The tuner samples one size on an otherwise idle machine, which is not the
same thing as your program under load.

## What it costs you

### Allocation

`hasher` is a fixed-size value type and never touches the heap.
`parallel_hasher` allocates its window buffer once, at construction.
Among the conveniences, `to_hex()` returns a `std::string` and
`to_hex_chars()` returns a `std::array<char, 65>` without allocating;
every span-filling overload writes into a buffer you own.

### Stack

The input is split into parts that the scheduler's agents pull as they
go, and each part's 32-byte chaining value waits on the calling thread's
stack. A `stack_budget` template argument sets how much stack that table
may take, 32 KiB (1024 parts) by default. Where stacks are small, give it
less and the input is split into fewer, larger parts. A budget that is
not a multiple of 32 bytes, or that holds fewer than two parts, does not
compile:

```cpp
blake3pp::digest d = blake3pp::hash<blake3pp::stack_budget{1024}>(buffer, sched);   // 32 parts
```

Code that knows its calling thread's stack can check the budget against
it at compile time:

```cpp
constexpr blake3pp::stack_budget budget{1024};
static_assert(budget.bytes <= CONFIG_MAIN_STACK_SIZE / 4);   // e.g. on Zephyr
```

The budget covers that table and nothing else. Each part is reduced on
the agent that took it, by a recursion that holds one chaining-value
buffer per level: 2 KiB per level while the build contains a 16-wide
kernel, since the buffer is sized for the widest variant compiled rather
than the one that runs, and as many levels as halving the part takes to
reach twice the running variant's degree in chunks.

#### What turning it down actually does

**It moves stack, it does not save it.** A smaller budget shrinks the
table on the calling thread and enlarges the frame on every agent,
because the parts get bigger and that recursion goes deeper. The total
across the program does not fall; it relocates from one thread to
several.

**Nothing catches an undersized thread.** The checks on `stack_budget`
are `consteval` and reject a malformed budget — not a multiple of 32
bytes, or fewer than two parts. They cannot know how much stack your
threads have, and where this header is compiled nobody does. Overflowing
an agent's stack is a crash or worse, not an error you can handle, which
is why the freestanding page gives the frames in bytes rather than
advice.

**Throughput can go before stack does.** Agents pull parts from a shared
counter, so parts are also the unit of load balancing. A budget that
yields fewer parts than the scheduler has agents leaves agents with
nothing to take, and the slowest part sets the finish time.

If the depth is the problem, `BLAKE3PP_SUBTREE_FOLD=<levels>` removes
that term altogether at 32 bytes per level, which makes an agent's stack
independent of the input size. The measured frames, the formulas and a
worked Zephyr case are in
[Freestanding and RTOS builds](freestanding.md#stack-requirements).

`keyed_hash`, `derive_key`, `update_file` and `hash_file` take the same
argument, and `parallel_hasher` takes it as its second template
parameter.

### How failures arrive

Every I/O entry point has a throwing form and a `std::error_code` form,
mirroring the standard library, so a caller branches or catches as it
prefers.

Nothing else in the library reports failure. Hashing cannot fail. A
variant the CPU cannot run falls back to one it can. Malformed hex
counts as no match.

## Expert seams

For engines that compute subtrees externally — custom pipelines, the I/O
layer itself — `hasher::push_subtree_cv()` absorbs an externally-computed
subtree chaining value, and `hasher` and the parallel `hash()` accept a
caller-supplied kernel table. That seam is how the benchmark plugs
upstream's hand-written assembly into this pipeline.
