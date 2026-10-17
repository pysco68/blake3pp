# Building

[← blake3pp](../README.md)

Three presets cover most of what you will want: a current GCC, a current
clang, and the C++20 polyfill path. Everything past that is the matrix
that proves the kernels, the remote-execution driver, and the switches
that let you re-measure the tuning decisions on your own hardware.

```bash
cmake --preset linux-gcc16-cxx26 && cmake --build --preset linux-gcc16-cxx26
ctest --preset linux-gcc16-cxx26
```

Any name from `cmake/toolchains/` works as a preset (see
`CMakePresets-toolchains.json`). Test presets exist for:

- `linux-gcc16-cxx26` and `linux-clang22-cxx26`
- `linux-clang18-cxx20-libstdcxx`: the C++20 polyfill path
- the two `-asan` variants
- `linux-clang22-cxx23-fuzzer`: the differential fuzz harness
  (`tests/fuzz_paths.cpp`), which feeds the same bytes through every path
  the library has and compares them. Its ctest leg is a few thousand
  inputs; a campaign is `blake3pp_fuzz_paths -max_total_time=<s> <corpus>`.
  libFuzzer's runtime is built against libstdc++, which is why this lane
  is not a libc++ one.

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
and the static musl set `linux-{,arm64-,riscv64-,ppc64le-,s390x-}zigmusl-cxx23-static`)
run their whole test suites under qemu-user with a selectable vector
length, so one build exercises every SVE VL or RVV VLEN; the emulator
recipes are in `docker/README.md` too. Two kernel sets are opt-in:

- `-DBLAKE3PP_SVE_ALL_VARIANTS=ON` adds the SVE variants matching no
  shipping silicon (emulator targets)
- `-DBLAKE3PP_XTHEAD_KERNEL=ON` compiles the hand-written T-Head
  XTheadVector (draft RVV 0.7.1) kernel, which only T-Head's qemu fork
  can execute

## Provider and polyfill selection

Two cache variables, each an explicit choice with an `auto` default,
pick what the SIMD facade and the sender/receiver pipeline compile
against. `cmake/StdFeatures.cmake` is the sole authority: it probes the
active toolchain by compiling real usage rather than trusting
feature-test macros, and the resolved choice is what every preset
above actually builds with. The reasoning behind the defaults and the
toolchain-specific overrides is in [Polyfills](polyfills.md); this is
the configure-time surface.

```
-DBLAKE3PP_SIMD_PROVIDER=auto|std|std-experimental|xsimd
-DBLAKE3PP_EXECUTION_PROVIDER=auto|std|beman|stdexec
```

`BLAKE3PP_SIMD_PROVIDER=auto` tries native `std::simd` (C++26 `<simd>`)
first, falls back to the Parallelism TS v2 `std::experimental::simd`,
and falls back again to xsimd, which is always buildable since it is a
fetched header-only dependency with no toolchain requirement of its
own. `std` and `std-experimental` each fail the configure outright if
the probe behind them does not pass, rather than silently substituting
something else; `xsimd` skips the std probes entirely.

`BLAKE3PP_EXECUTION_PROVIDER=auto` picks `std::execution` when the
standard library has a usable one and NVIDIA stdexec otherwise;
beman.execution is never chosen by `auto` and needs the explicit
`-DBLAKE3PP_EXECUTION_PROVIDER=beman` (it also needs C++23 or newer,
which the project's C++20 presets cannot offer). `BLAKE3PP_SIZED_PARALLEL_SCHEDULER`
(off by default) additionally builds `blake3pp::parallel_backend`, the
sizable process-wide scheduler, and under beman builds its default
backend out so the program keeps one definition of it.

A configure reports every probe and the resolved choice regardless of
whether either variable was touched:

```
-- blake3pp: BLAKE3PP_HAS_STD_SIMD = yes
-- blake3pp: simd provider choice = auto
-- blake3pp: BLAKE3PP_HAS_STD_SENDERS = no (polyfill path)
-- blake3pp: execution provider = stdexec (BLAKE3PP_EXECUTION_PROVIDER=auto)
```

and a running binary reports which provider it was actually built with,
independent of the configure log:

```cpp
blake3pp::simd_provider();       // "std::simd", "std::experimental::simd",
                                  // "xsimd", or "none (scalar only)"
blake3pp::execution_provider();  // "std::execution", "beman.execution",
                                  // "stdexec", or "none"
```

(`blake3ppsum --version` prints both; so does the wasm module's
`b3pp.simd_provider()`/`b3pp.execution_provider()`, see
[Architectures and kernels](architectures.md).) A handful of
cross-compiling toolchains pin `BLAKE3PP_SIMD_PROVIDER` themselves
instead of leaving it on `auto` --
`linux-riscv64-gcc15-cxx23`, `linux-ppc64le-gcc15-cxx23`,
`linux-s390x-gcc15-cxx23` and the ppc64le/s390x zigmusl presets set it
to `xsimd`, and `linux-mips64el-gcc14-cxx23` sets `std-experimental` --
because each one's vector kernel already pins a provider for reasons
specific to that ISA which `auto`'s probes cannot see, and leaving the
project-wide variable on `auto` would make `simd_provider()` report a
provider the kernel does not actually use.

Overriding either variable on a preset that does not need it is for
re-measuring a provider choice, the same spirit as the kernel tuning
switches below.

## Coverage

The `linux-clang22-cxx26-coverage` preset instruments at the plain
matrix's optimisation level, so the numbers describe the code that
ships:

```bash
tools/tc linux-clang22-cxx26 -- bash -c '
  cmake --preset linux-clang22-cxx26-coverage &&
  cmake --build --preset linux-clang22-cxx26-coverage &&
  LLVM_PROFILE_FILE="$PWD/build/linux-clang22-cxx26-coverage/profraw/%p.profraw" \
    ctest --test-dir build/linux-clang22-cxx26-coverage'
llvm-profdata merge -sparse build/linux-clang22-cxx26-coverage/profraw/*.profraw \
  -o build/linux-clang22-cxx26-coverage/all.profdata
llvm-cov report build/linux-clang22-cxx26-coverage/tests/blake3pp_tests \
  -instr-profile=build/linux-clang22-cxx26-coverage/all.profdata src/io include
```

If an LLVM tool sits there burning no CPU, it is waiting on a
descriptor, not working: the base image sets `DEBUGINFOD_URLS` to
Canonical's server, the connect never completes from here, and the tool
polls for it for ever. The devcontainer sets the variable empty for that
reason; outside it, put `env -u DEBUGINFOD_URLS` in front of the
command.

## Building through cmake-re

Every containerized preset also builds through [tipi cmake-re]. It runs
the compile actions on a remote-execution cluster, EngFlow through
reclient, and serves repeats from its action cache. The tests, emulator
matrices and artifacts come out the same as with plain cmake.

cmake-re ships in every x86_64 toolchain image, with
`TIPI_DISTRO_MODE=none` so that it drives the image's own compilers. It
knows nothing about presets, so `tools/preset-args.py` unrolls one into
plain configure arguments:

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

### The toolchain folders

Each toolchain lives in its own folder under `cmake/toolchains/`, with
the `.pkr.js` and `.layers.json` that name its image at the content tag
and the manifest digest that tag resolves to. That folder is the
environment cmake-re copies when it is not told `--host`, and the cluster
pulls the same image for the compile actions.

`tools/gen-environments.py` writes those files. `--check` reports stale
ones, and the digests come from the registry or from a `--digests` map.
`BLAKE3PP_TC_REGISTRY` and `BLAKE3PP_TC_IMAGE_TEMPLATE` retarget them at
a registry mirror the cluster can reach.

CI does not depend on them being current:

- Every run writes the tag and digests it builds with into them, and
  builds on that.
- When the committed files lag, a run of `main` opens one draft pull
  request proposing the update. It is `ci/toolchain-environments`,
  updated in place while the tag keeps moving, and marking it ready for
  review runs CI on it.
- A release built on injected files is named and annotated as a dirty
  build.

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

## Kernel tuning switches

Nine cache variables turn measured kernel optimizations on or off. Each
is `auto|on|off` and defaults to `auto`, which is the fastest setting on
every machine it has been measured on. **These are not performance
options to tune, they are measurement controls**, and turning one off
makes the library slower. They exist so a result can be re-checked on
hardware its original measurement did not cover; the resolved value is
reported at configure time when it is not the default.

| variable | default | off means |
|----------|---------|-----------|
| `BLAKE3PP_KERNEL_INLINE_ENFORCEMENT` | on, all targets | Drop `always_inline`/`__forceinline` from the round core. Every compiler measured then outlines it (clang the whole `all_rounds`, GCC the `index_sequence` lambda), costing 6-40% depending on compiler and variant. |
| `BLAKE3PP_KERNEL_SRI_ROTATE` | on, aarch64 | Spell rot12/rot7 as the generic shift-or, which selects `shl`+`usra` instead of `shl`+`sri`. `sri` is worth ~6% on Apple M2 (clang 22), 12% on Neoverse V2 and 8% on Neoverse N1 (clang 21, static build), measured as alternating A/B pairs. |
| `BLAKE3PP_KERNEL_STAGED_ROUNDS` | on, aarch64 | Run each round as sequential `g` calls instead of quartet-staged. A small win on Apple M2 / clang 22, and provably inert on GCC 15 (same schedule, different register names). Loses on x86, where it is off regardless. |
| `BLAKE3PP_KERNEL_XAR_ROTATE` | on, SVE2 variants | Spell `rot(x ^ y)` as `eor` + rotate instead of one fused `XAR`. Off costs the SVE2 kernels their entire margin over NEON: measured 1.89 vs 1.61 GiB/s on Neoverse V2 (GCP Axion). |
| `BLAKE3PP_KERNEL_SHUFFLE_TREE` | on, all SIMD targets | Stage every transpose through a scalar array and spell the byte rotates as shift-or, the code before the shuffle-tree bypass. Off costs SSE4.2 about half its throughput on clang. |
| `BLAKE3PP_KERNEL_ROT16_PER_COMPILER` | on, x86 clang | Spell rot16 as the byte shuffle on clang too, which LLVM lowers to `pshuflw`+`pshufhw` where the shift-or folds to one `pshufb`. The pre-split spelling, kept for the A/B. |
| `BLAKE3PP_KERNEL_VROR_ROTATE` | on, RVV Zvbb variants | Spell the rotate as the 4-op shift-or (base RVV has no rotate) instead of `vxor`+`vror`. The RISC-V counterpart of the XAR switch; not yet measured on Zvbb silicon, which this switch makes possible. |
| `BLAKE3PP_KERNEL_VEXT_W8_GATHER` | on, GCC at eight lanes with AVX-512 available | Transpose through the shuffle network there too. GCC lowers the network's cross-lane stage to an index-register permute per stage when the register is a raw vector extension and AVX-512 is in reach, so the staging gather is faster in exactly that corner; the switch is the A/B, and in AVX2-only builds the network wins by 19%. |
| `BLAKE3PP_KERNEL_PIN_MESSAGE` | on, AVX-512 width on GCC and clang | Hold the message block in registers. At sixteen lanes the state takes half the register file and the message cannot fit beside it, so letting its address escape once makes the compiler read it through memory operands: worth 7.8% on the kernel row and 8.5% end-to-end on Zen 5, and 2-4% at 32 threads (3-6% measured on the chunk shape by itself). |

```bash
# Re-run the inlining A/B on a new machine:
cmake --preset macos-clang22-cxx26 -DBLAKE3PP_KERNEL_INLINE_ENFORCEMENT=off
```

Two benchmark comparisons are build options of their own.
`BLAKE3PP_BENCH_UPSTREAM` (on) builds the upstream BLAKE3 C library, so
every run shows blake3pp's variants against the hand-written assembly on
the same machine. `BLAKE3PP_BENCH_COMPARE` (off, needs cargo) builds the
reference implementation's own rayon bench from `bench/compare/`, one
binary per compile-time SIMD cap, which is the comparison against the
implementation as its users get it.

`BLAKE3PP_KERNEL_EXTRA_FLAGS` (a semicolon-separated list) appends raw
compiler flags to the kernel TUs only, for one-off flag trials that have
not earned a switch.

Two more cache variables shape the tree reduction rather than the kernel.

`BLAKE3PP_SUBTREE_GROUP=<power of two>` sets how many chunk batches a
reduction hashes before it compresses their parents; 8 is the default. The
kernel's chunk loop and its one-block parent call do not stay hot
together, so the burst pays for the locality it gives up. Measured on Zen 5
AVX-512 with the parent work unchanged and only its grouping varied:
6.7 GiB/s at one batch per burst, 7.6 at eight, 7.8 at thirty-two.

`BLAKE3PP_SUBTREE_FOLD=<levels>` goes the other way, for targets that
cannot spare the burst's staging buffers. The frames are in
[Freestanding and RTOS builds](freestanding.md#stack-requirements).

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
