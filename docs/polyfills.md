# Polyfills

[← blake3pp](../README.md)

The library is C++20 at its floor and uses C++26 facilities where the
toolchain ships them: `std::simd` for the kernels, `std::execution` for
the sender/receiver tree reduction. Two independent axes, SIMD and
execution, each resolve to one of several providers that spell the same
operations differently. A polyfill is never a second implementation: it
is the same source compiled against a different vocabulary for the same
operations, chosen once at configure time.

## The contract

A provider choice may change which library the symbols come from and
which types the compiler sees. It may never change:

- the digest a given input produces,
- the public API (`hasher`, `digest`, `hash()`, and the rest of
  `<blake3pp/blake3pp.hpp>`),
- which kernels a build carries (that is a separate axis, the
  architecture list in [Architectures and kernels](architectures.md)).

What it does change is which internal spelling compiles: inside a kernel
TU, a `u32v` wide-word type backed by `std::simd::vec`, xsimd's
`batch`, or a GNU vector-extension type; inside the parallel pipeline,
`schedule`/`bulk`/`sync_wait` backed by `std::execution`,
`beman::execution`, or `stdexec`. Both facades hide the choice behind a
single internal namespace (`blake3pp::kern::<arch>::u32v`,
`blake3pp::ex`), so kernel and pipeline source never sees which provider
it got.

## Why an explicit choice, not pure detection

Each axis has a cache variable with an `auto` default (below). Nothing
about either axis is decided by silently preferring whatever a
toolchain happens to expose, for two reasons.

First, feature-test macros lie in the window that matters most: a
toolchain trunk can ship a working facility before it defines the
macro that is supposed to announce it. Detection compiles real usage
under the exact standard, standard library and toolchain flags of the
current configure, and trusts the compile, not the macro.

Second, a toolchain can pass that probe and still be the wrong answer.
libstdc++'s `std::experimental::simd` deduces a native vector width from
a fixed list of ISAs; on an architecture the list has never heard of
(MIPS/MSA, at the time of writing) the deduced width is 1 and no vector
instruction is ever emitted, even though the probe compiled cleanly and
the compiler can target that ISA perfectly well. A toolchain-specific
override exists for exactly this gap, and only an explicit choice can
express "this probe is not a good enough answer here" — detection
alone cannot tell a width-1 fallback from a real one.

`auto` is still the right default almost everywhere: it is the fastest
answer the project knows of for a given toolchain, and the explicit
settings exist for the toolchains that need to say otherwise and for
anyone re-measuring the choice.

## SIMD providers

Selected by `BLAKE3PP_SIMD_PROVIDER` (`auto`, `std`, `std-experimental`,
`xsimd`); see [Building](building.md#provider-and-polyfill-selection)
for the cache variable and what a configure reports. A fourth path, the
raw vector-extension provider, is not project-wide: it exists only as a
per-kernel override for ISAs none of the other three reach.

| provider | needs | `auto` picks it when |
|---|---|---|
| `std::simd` | C++26 `<simd>` | the standard library has a usable one |
| `std::experimental::simd` | the Parallelism TS v2 header | no `std::simd`, and the TS header compiles and vectorizes |
| xsimd | nothing from the toolchain (fetched, header-only) | neither std facility is usable |
| vector extensions (GNU/Clang `vector_size`) | never project-wide | pinned per kernel, for an ISA no other provider's width logic reaches |
| none (scalar) | nothing | a build's only kernel is the scalar one |

`auto` tries `std::simd` first, falls back to
`std::experimental::simd`, and falls back again to xsimd, which is
always available since it carries no toolchain requirement of its own.
A build can also carry no SIMD provider at all: a scalar-only build
(the single-file amalgamation below C++26, for instance) needs none,
and `blake3pp::simd_provider()` reports `"none (scalar only)"`.

Two things override the project-wide choice for one kernel TU, both
narrower mechanisms than the cache variable above:

- **The vector-extension provider**, pinned where the std providers'
  width deduction fails outright. libstdc++'s
  `std::experimental::native_simd<uint32_t>` reports width 1 on an ISA
  its internal list does not cover (measured on MIPS64/MSA, GCC 14,
  where the same operations spelled over an explicit 16-byte
  vector-extension type compile to six MSA instructions instead of
  none); xsimd has no backend there either. Ordinary compiler-driven
  vectorization over a `vector_size` type reaches any ISA the compiler
  itself targets, with no width guess to get wrong.
- **An xsimd pin**, where a std provider is usable but wrong for a
  different reason: libstdc++'s `std::experimental::simd` SVE backend
  keeps index tables whose dynamic initializers are SVE instructions
  that run at **load time**, which is an instant illegal-instruction
  fault on any non-SVE machine running the same binary. That defeats
  runtime dispatch outright, so every SVE kernel variant pins xsimd
  (initializer-free there) regardless of the project-wide setting.

Three cross-compiling toolchains (riscv64, ppc64le, s390x) set
`BLAKE3PP_SIMD_PROVIDER` to `xsimd` for the whole build rather than
leaving it on `auto`, for a third reason again: none of the three has
an `std::experimental::simd` ABI worth trusting on that architecture,
and each one's vector kernel (RVV, VSX, VXE) already pins xsimd for
itself regardless. Stating the project-wide choice to match keeps
`simd_provider()`'s answer truthful instead of reporting a provider
the vector kernel does not actually use.

## Execution providers

Selected by `BLAKE3PP_EXECUTION_PROVIDER` (`auto`, `std`, `beman`,
`stdexec`); see
[Building](building.md#provider-and-polyfill-selection) for the cache
variable.

| provider | floor | `auto` picks it when |
|---|---|---|
| `std::execution` | a standard library implementing `<execution>` senders | the probe (`std::execution::just`) compiles |
| NVIDIA stdexec | C++20 | the probe does not compile |
| [beman.execution] | C++23 | never automatically |

`auto` never selects beman.execution: its upstream labels itself
pre-production, so reaching it is always an explicit
`-DBLAKE3PP_EXECUTION_PROVIDER=beman`. Between the two providers `auto`
does choose, `std::execution` wins whenever the standard library ships
one that passes the probe (a `std::execution::just` compile), and
stdexec is the fallback otherwise. No toolchain in the current test
matrix passes that probe yet, so every preset today builds against
stdexec under `auto`; the `std::execution` branch exists and compiles,
but is unverified against a real standard-library implementation until
one exists to measure it against.

A build can carry no execution provider at all (`"none"` from
`execution_provider()`): the multi-core entry points, and the headers
that declare them, do not exist in that configuration.

[beman.execution]: https://github.com/bemanproject/execution

## What a provider choice does not decide

The two axes are independent of each other and of everything else the
build decides. Choosing xsimd over `std::simd` does not add or remove a
kernel, change which architectures dispatch can select, or alter a
single output byte: the official BLAKE3 test vectors pass identically
under every provider combination the test matrix covers, and the
differential fuzz harness (`tests/fuzz_paths.cpp`,
[Building](building.md)) feeds the same bytes through every path the
library has and compares them against each other specifically to keep
that true as the source changes.

## Linkage: one provider per program

The execution provider is a compile-time choice baked into object code,
not a runtime switch, which makes it an ODR hazard the moment a header
that names it reaches more than one translation unit compiled against
different providers. `include/blake3pp/detail/ex_compat.hpp` documents
the rules a custom sender, scheduler or receiver has to follow to stay
safe under every provider — in particular, that a namespace-scope
template must never hold a provider's sender type unless that sender's
own operation types are named among the template's arguments. Getting
this wrong does not fail to compile and does not produce a wrong
digest; it produces two translation units that agree on a type's
mangled name while disagreeing on its layout, caught only under a
sanitizer as a vptr mismatch. `tests/provider_linkage.cpp` and its
second translation unit exist to keep that failure mode caught by the
suite rather than found again by hand.

The practical rule for a consumer follows directly: a program links
against exactly one execution provider, and every translation unit that
includes `<blake3pp/parallel.hpp>` or `<blake3pp/parallel_io.hpp>` (or
the umbrella header) must see the same one blake3pp was built with. The
library's installed package exposes which provider that is and arranges
the matching macro automatically; see
[Integration](integration.md#installing-and-find_package) for the
consumer-side mechanics.

The SIMD facade carries no equivalent hazard: it is kernel-TU internal
and never crosses the kernel boundary (`src/kernel/simd_facade.hpp`),
so no public header names a SIMD provider type.

## Where the polyfills come from

Every polyfill is a pinned dependency fetched through `FetchContent`
(xsimd, stdexec) or hfc's hermetic wrapper around it (beman.execution,
which needs isolation from its own network-reaching configure step);
none of them needs to be installed on the machine beforehand, and none
is resolved by a version range. [Building](building.md) covers the
cache variables that select a provider and what a configure reports;
[Integration](integration.md) covers what an installed package freezes
and what a consumer still has to provide for itself.
