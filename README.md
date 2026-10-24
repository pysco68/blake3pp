# blake3pp

[![ci](https://github.com/pysco68/blake3pp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/pysco68/blake3pp/actions/workflows/ci.yml)
[![release](https://github.com/pysco68/blake3pp/actions/workflows/release.yml/badge.svg?branch=main)](https://github.com/pysco68/blake3pp/actions/workflows/release.yml)
[![standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2023%20%7C%2026-blue)](docs/building.md)
[![license](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

An attempt at the *fastest possible* (`std::execution` + `std::simd` + direct I/O) **clean** (no assembly / intrinsics if avoidable) C++26&ast; BLAKE3 implementation that hashes from memory and files.

(*) Shipping all of that today by using polyfills that make it *just work* with most C++20 & up toolchains.

```cpp
#include <blake3pp/blake3pp.hpp>
#include <print>

int main(int, char** argv) {
  const auto digest = blake3pp::hash_file(argv[1], blake3pp::get_parallel_scheduler());
  std::println("{}  {}", digest.to_hex(), argv[1]);
}
```

[![try it on Compiler Explorer](https://img.shields.io/badge/try%20it-Compiler%20Explorer-67c52a?logo=compilerexplorer&logoColor=white)](https://pysco68.github.io/blake3pp/try/)

```cmake
include(FetchContent)
FetchContent_Declare(blake3pp
  GIT_REPOSITORY https://github.com/pysco68/blake3pp.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(blake3pp)
target_link_libraries(your_app PRIVATE blake3pp::blake3pp)
```

That call opens the file with direct I/O, keeps several windows in flight,
hashes with the widest kernel the processor turns out to have and spreads
the work over all cores the system has.

Underneath, each window of the file is one `std::execution` sender chain --
read, compress over your scheduler, absorb -- driven by the thread that
called `hash_file()`, and a tree reducer takes the windows back in whatever
order they finish. [Inside the pipeline](docs/pipeline.md) has the chain,
the rules that hold it together, and the write direction.

The whole specification is here, not just the digest:

- **Three modes.** Plain hashing, keyed hashing for a MAC or PRF, and
  `derive_key` for context-separated subkeys.
- **Output of any length.** 32 bytes, or an extended stream seekable in
  constant time.
- **One shot or incremental**, and **sequential or over a scheduler**.

Files are a first-class input, not a loop you write.

The binary release contains two [command-line tools](docs/tools.md) built on the library: `blake3ppsum` and `blake3ppgen`.

`blake3ppsum` is a core-tool style hashing tool you can use just like `sha256sum` but
exposes every BLAKE3 mode: keyed hashing, derive_key and extended output.

`blake3ppgen` uses BLAKE3's extended output mode to generate deterministic arbitrary
length, seekable streams of data which is perfect for test data generation.

<details>
<summary>How fast is `blake3ppsum`?</summary>

| machine | the drive | `b3sum` | `blake3ppsum` |
|---|---|---|---|
| Apple M2, macOS, 1 GiB | 2.92 GiB/s | 0.93 | **2.68** |
| EPYC 8124P, 4 × PCIe 5, Linux 7.0, 32 GiB | 43.2 GiB/s | **30.2** | 25.4 |
| the same, at `--window 64 --qd 4` | 51.0 GiB/s | — | **39.8** |

The drive column is the same reader with hashing switched off, on the
same machine and the same file: the ceiling any tool on that box is
working against. The wider window raises both that ceiling and the
result.
</details>

## A few more examples

```cpp
// A buffer, one line. Anything span-like, or a string_view.
blake3pp::digest d = blake3pp::hash(payload);
std::println("{}", d.to_hex());

// Verification, constant-time, with malformed hex simply not matching.
if (blake3pp::hash(payload).matches(expected_hex)) { /* verified */ }
```

```cpp
// Streaming: a fixed-size value type, no heap, and finalize() does not
// consume the hasher.
blake3pp::hasher h;
while (auto block = source.next_block()) h.update(*block);
blake3pp::digest checkpoint = h.finalize();   // ...and keep going
h.update(trailer);
blake3pp::digest full = h.finalize();
```

```cpp
// A MAC over a file, at pipeline speed: the key turns the same call into
// keyed mode.
std::array<std::byte, 32> key = load_secret_key();
auto tag = blake3pp::hash_file(path, blake3pp::get_parallel_scheduler(),
                               {.mode = blake3pp::hash_mode::keyed(key)});

// Purpose-bound subkeys from one master secret, separated by context.
auto session = blake3pp::derive_key("example.com 2026 tls session", master);
```

```cpp
// Extended output: any length, and seekable in constant time.
blake3pp::output_reader r = blake3pp::hasher{}.finalize_xof();
r.seek(10'000'000'000);      // byte ten billion costs what byte zero costs
r.fill(chunk);
```

All of it, with the edge cases, in [the API](docs/api.md). Or
[run it in your browser](https://pysco68.github.io/blake3pp/try/): one
amalgamated file on Compiler Explorer, hashing on one core and on all of
them.

## Platforms

One binary per platform carries every kernel listed for it and picks the
best one the running CPU can execute; the I/O path is chosen the same
way, per feature, at startup.

| platform | SIMD kernels | async I/O | cache bypass |
|---|---|---|---|
| Linux x64 | SSE4.2, AVX2, AVX-512 | io_uring | `O_DIRECT` |
| Linux aarch64 | NEON, SVE 256/512, SVE2 128 | io_uring | `O_DIRECT` |
| Linux riscv64 | RVV 1.0 at VLEN 128/256/512, each with a Zvbb twin; XTheadVector (draft 0.7.1) | io_uring | `O_DIRECT` |
| Linux ppc64le | VSX | io_uring | `O_DIRECT` |
| Linux s390x | VXE (z14+, **big-endian**) | io_uring | `O_DIRECT` |
| Linux mips64el | MSA — emulator-tested only, never released | io_uring | `O_DIRECT` |
| Linux loongarch64 | LSX, LASX — emulator-tested only | io_uring | `O_DIRECT` |
| Windows x64 | SSE4.2, AVX2, AVX-512 | I/O ring (Windows 11) or IOCP | `FILE_FLAG_NO_BUFFERING` |
| Windows arm64 | NEON, SVE 256/512, SVE2 128 under clang-cl; `cl` ships scalar alone | I/O ring or IOCP | `FILE_FLAG_NO_BUFFERING` |
| macOS arm64 | NEON | GCD (libdispatch) | `F_NOCACHE` |
| FreeBSD amd64 | SSE4.2, AVX2, AVX-512 | POSIX AIO on a kqueue | `O_DIRECT` |
| FreeBSD arm64 | NEON, SVE 256/512, SVE2 128 | POSIX AIO on a kqueue | `O_DIRECT` |
| wasm32 | SIMD128 | none: synchronous stdio | none |
| any other POSIX | scalar, plus whatever its architecture has | none: synchronous `pread`/`pwrite` | `O_DIRECT` where the OS has it |

Every build also carries a scalar kernel as the last rung and as the
oracle the wide kernels are tested against. Each feature degrades
independently at runtime: an engine the OS refuses falls back to
synchronous `pread`/`pwrite` and then to stdio, a filesystem that will
not serve unbuffered reads is read through the cache, and the tools
print the rung that actually ran rather than the one that was asked for.

CI releases twelve archives: the six static Linux ones, both Windows
clang-cl zips, macOS arm64, the wasm bundle and both FreeBSD tarballs.
mips64el is built and emulator-tested on every run but never packaged,
and the `cl` arm64 build is compile-proven only. Both FreeBSD
architectures have a full lane: every run cross-builds them with zig
and then boots a real FreeBSD under qemu on the runner to run the
suite, the I/O-rung digest checks and a bench on it — amd64 under KVM
on a 14.x guest, arm64 under TCG on a 15.x one, because no CI runner
can KVM an aarch64 guest and because FreeBSD only publishes the SVE
capability bit from 15.0 on. The FreeBSD archives are the only ones
here that are dynamically linked (zig refuses `-static` for that
target) and they require FreeBSD 14.0 or newer.

More details: [Architectures and kernels](docs/architectures.md) for the
kernels, one row per variant with its runtime check, and
[Direct I/O](docs/direct-io.md) for the engines and the exact names each
rung reports.

## Documentation

All of this, rendered and searchable, with a version for every release:
**[pysco68.github.io/blake3pp](https://pysco68.github.io/blake3pp/)**.

| | |
|---|---|
| [Examples](examples/) | ten standalone programs, one per aspect of the library |
| [The API](docs/api.md) | the guide across the library: what you compute, how you feed it, what runs it, what it costs |
| [Inside the pipeline](docs/pipeline.md) | how a file becomes sender chains: one window's chain, the rules that hold it, and the tree reducer |
| [Many files at once](docs/async.md) | EXPERIMENTAL: file hashing as senders over a context you drive |
| [Direct I/O](docs/direct-io.md) | what `direct_io` asks for on each platform, the alignment contract, and when buffered is the right call |
| [Architectures and kernels](docs/architectures.md) | every kernel a build carries, how it is tested, and what has no kernel yet |
| [Polyfills](docs/polyfills.md) | the SIMD and execution providers: what may stand in for what, and what a choice cannot change |
| [Performance tracing](docs/tracing.md) | where a file run's time went, and reading one in the tools |
| [Command-line tools](docs/tools.md) | `blake3ppsum` and `blake3ppgen` |
| [Integration](docs/integration.md) | vendoring, installing, and `find_package` |
| [Building](docs/building.md) | presets, the toolchain images, cmake-re, and the kernel switches |
| [Freestanding and RTOS builds](docs/freestanding.md) | no OS, no allocator and how to use `blake3pp` with only a few hundred bytes of stack |
| [Static Linux binaries](docs/static-linux-binaries.md) | one file per architecture that runs on any distro |
| [Reference](https://pysco68.github.io/blake3pp/latest/reference/) | every declaration in the public headers, generated from their own doc comments |

## Using the library

blake3pp is normally consumed **from source**: the FetchContent block above, or a
submodule plus `add_subdirectory`. Tests, benchmarks and tools stay out of consuming
builds by default.

The polyfill dependencies (xsimd, stdexec) are fetched and version-pinned
by blake3pp's own build where the toolchain lacks the C++26 facilities;
nothing to install. Building from source is recommended at 0.x because the
feature probes run against *your* toolchain and standard-library
combination; an installed package freezes the choices made on the machine
that built it. Installing is supported too, see
[Integration](docs/integration.md#installing-and-find_package).

Prebuilt binaries for every supported target come out of CI; see
[Integration](docs/integration.md#prebuilt-releases).

Everything lives in `namespace blake3pp`, and
`#include <blake3pp/blake3pp.hpp>` gets you all of it. Compile-cost-aware
consumers can pick granular headers instead:

| header                       | provides | needs an execution provider |
|------------------------------|----------|:---:|
| `<blake3pp/blake3pp.hpp>`    | umbrella: everything below | yes |
| `<blake3pp/dispatch.hpp>`    | `arch` introspection, SIMD variant selection | no |
| `<blake3pp/core.hpp>`        | `digest`, `hasher`, one-shot `hash()` | no |
| `<blake3pp/io.hpp>`          | `update_file()`, `hash_file()` and sequential `write_file()`, the async direct-I/O pipeline | no |
| `<blake3pp/parallel.hpp>`    | multi-core `hash()`, `parallel_hasher`, multi-core XOF `fill()` | yes |
| `<blake3pp/parallel_io.hpp>` | `update_file()`, `hash_file()` and `write_file()` over a scheduler (the two combined) | yes |
| `<blake3pp/async_io.hpp>`    | EXPERIMENTAL: the same as senders over a context you drive, for many files on one arena | yes |
| `<blake3pp/trace.hpp>`       | `trace_buffer`: per-window timing records for `update_file()` | no |

The last column is the one that matters when you install blake3pp rather
than build it: only the scheduler-taking headers include an execution
library (stdexec, beman.execution, or `<execution>`). Hashing buffers and
hashing files sequentially compile against the standard library alone.
The rule behind the split, and which header owns which dependency, is in
[Integration](docs/integration.md#header-layout).

### Guarantees and caveats

- The sequential compute paths never allocate: `hasher` is a flat value
  type and subtree buffers live on the stack. Everything that fans out
  allocates once, when a run is planned: the I/O pipeline takes its window
  arena, the windows that carry the part tables, the reducer's nodes and
  the operation cells -- once per call for the blocking entry points, once
  per context for the async ones -- and a part table grows its lane array
  on the first window that needs the size. `parallel_hasher` also
  allocates its accumulation window at construction, and each bulk launch
  allocates whatever the execution provider needs (stdexec: one small task
  array per launch).
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
  end to end for valid input. Two kinds of call are not: the ones that
  build a `std::string` (`digest::to_hex()` and free `to_hex(bytes)`; the
  span-filling `to_hex_chars()` is the allocation-free form), and the
  checked preconditions of the expert seams (`push_subtree_cv()`,
  `to_hex(bytes, out)` and the reducer), which throw
  `std::invalid_argument` or `std::length_error` in every build rather
  than write a wrong digest. The I/O layer throws `std::system_error`, or
  reports through the `std::error_code` overloads, which also fold
  allocation failure into an error code. The scheduler-taking parallel
  APIs propagate whatever the execution provider raises, plus
  `bad_alloc`.
- Full BLAKE3 spec surface: plain, keyed and derive_key modes, each with
  arbitrary-length (XOF) output; every mode is verified against all 131
  output bytes of the official test vectors.

## License

Copyright 2026 Yannic Staudt. Apache License 2.0 with LLVM Exceptions; see
[LICENSE](LICENSE).
