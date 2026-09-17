# Building

[← blake3pp](../README.md)

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

## Building through cmake-re

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

## Kernel tuning switches

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
| `BLAKE3PP_KERNEL_SRI_ROTATE` | on, aarch64 | Spell rot12/rot7 as the generic shift-or, which selects `shl`+`usra` instead of `shl`+`sri`. `sri` is worth ~6% on Apple M2 (clang 22), 12% on Neoverse V2 and 8% on Neoverse N1 (clang 21, static build), measured as alternating A/B pairs. |
| `BLAKE3PP_KERNEL_STAGED_ROUNDS` | on, aarch64 | Run each round as sequential `g` calls instead of quartet-staged. A small win on Apple M2 / clang 22, and provably inert on GCC 15 (same schedule, different register names). Loses on x86, where it is off regardless. |
| `BLAKE3PP_KERNEL_XAR_ROTATE` | on, SVE2 variants | Spell `rot(x ^ y)` as `eor` + rotate instead of one fused `XAR`. Off costs the SVE2 kernels their entire margin over NEON: measured 1.89 vs 1.61 GiB/s on Neoverse V2 (GCP Axion). |
| `BLAKE3PP_KERNEL_SHUFFLE_TREE` | on, all SIMD targets | Stage every transpose through a scalar array and spell the byte rotates as shift-or, the code before the shuffle-tree bypass. Off costs SSE4.2 about half its throughput on clang; see the kernel section of the talk notes. |
| `BLAKE3PP_KERNEL_ROT16_PER_COMPILER` | on, x86 clang | Spell rot16 as the byte shuffle on clang too, which LLVM lowers to `pshuflw`+`pshufhw` where the shift-or folds to one `pshufb`. The pre-split spelling, kept for the A/B. |
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
