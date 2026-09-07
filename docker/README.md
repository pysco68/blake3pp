# Toolchain images

One Docker image per compiler, so CI jobs and local matrix runs pull a small
prebuilt image instead of the old 8.6 GB kitchen-sink devcontainer. The
`cmake/toolchains/<name>/<name>.cmake` files are unchanged: they name compilers by bare
PATH name, which resolves inside the right image.

## The mapping

Each compiler sits on the Ubuntu release contemporary to it, so its binaries
carry the oldest practical **glibc floor** for that vintage. Compatibility is
one-directional by design: artifacts built on an older floor run in every
newer image, in the 26.04 devcontainer, and on any host with glibc >= the
floor. The reverse (running gcc16 output inside the 22.04 image) is
unsupported and unneeded.

| Image | Base | glibc floor | Serves presets | Extras |
|---|---|---|---|---|
| `toolchain-gcc14` | 24.04 | 2.39 | `linux-gcc14-*` | |
| `toolchain-gcc16` | 26.04 | ~2.42 | `linux-gcc16-*` (incl. asan/tsan/coverage) | gdb, valgrind, lcov/gcovr, Intel SDE |
| `toolchain-clang18` | 24.04 | 2.39 | `linux-clang18-*` | g++-14 tree for the libstdcxx pin |
| `toolchain-clang20` | 24.04 | 2.39 | `linux-clang20-*` | g++-14 tree; clang from apt.llvm.org noble-20 |
| `toolchain-clang22` | 26.04 | ~2.42 | `linux-clang22-*` (incl. asan/tsan/msan/fuzzer/coverage/cxx2c) | g++-16 tree, libfuzzer/libomp, gdb, valgrind, Intel SDE, **/opt/libcxx-msan** |
| `toolchain-arm64-gcc15` | 26.04 | (aarch64 sysroot) | `linux-arm64-gcc15-*` | qemu-aarch64 |
| `toolchain-riscv64-gcc15` | 26.04 | (riscv64 sysroot) | `linux-riscv64-gcc15-*` | qemu-riscv64 (RVV 1.0, `QEMU_CPU=max,vlen=<bits>`); opt-in Xuantie-qemu stage for XTheadVector (`--set riscv64-gcc15.args.WITH_XUANTIE_QEMU=1`, builds T-Head's fork in a pinned 22.04 stage; verify the 0.7.1 kernel with the freestanding `tests/xthead_verify.cpp` harness under `qemu-riscv64-xuantie -cpu c906fdv`; glibc binaries cannot run on the th CPU models, see the harness header) |
| `toolchain-ppc64le-gcc15` | 26.04 | (ppc64le sysroot) | `linux-ppc64le-gcc15-*` | qemu-ppc64le (`QEMU_CPU=power8/9/10`) |
| `toolchain-s390x-gcc15` | 26.04 | (s390x sysroot) | `linux-s390x-gcc15-*` | qemu-s390x (big-endian; `QEMU_CPU=max,vxeh=off` for the scalar fallback) |
| `toolchain-zig` | 26.04 | n/a (static musl) | `*zigmusl*` (x86_64, aarch64, riscv64), `tools/make-release.sh` | qemu-user, aarch64+riscv64 binutils strip, prewarmed zig cache (group-shared), the zig wrappers on PATH |
| `toolchain-emscripten` | 26.04 | n/a (wasm) | `wasm32-*` | node; the apt package's frozen sysroot cache covers pthread/wasm-eh/simd |

Registry: `ghcr.io/pysco68/blake3pp/toolchain-<name>`. The canonical tag
is **content-addressed**: `tree-<hash>` over the `docker/` git subtree,
`tools/build-msan-libcxx.sh` and the image workflow itself
(`tools/toolchain-image-tag.sh` prints your checkout's). ci.yml checks
whether that tag exists in GHCR and calls
`.github/workflows/toolchains.yml` to build it only on a miss: identical
content never rebuilds, and the test jobs always pull exactly the content
tag their checkout implies. `latest` is a convenience alias for local
`tools/tc` use, moved on main (fresh build or manifest retag) and by
manual `workflow_dispatch` refreshes.

## Daily use

From the (slim) devcontainer, via docker-outside-of-docker:

```sh
tools/tc linux-gcc14-cxx23                 # configure + build + ctest
tools/tc linux-clang22-cxx26-msan          # msan against the baked libc++
tools/tc linux-zigmusl-cxx23-static -- sh tools/make-release.sh
tools/tc --shell wasm32-emcc-cxx23
tools/tc --pull                            # refresh after a CI image push
```

`tc` mounts the workspace at `/workspaces/blake3pp` (the devcontainer's own
path), so build trees and
`compile_commands.json` are valid on both sides.

### cmake-re (every image)

The base recipe installs tipi's cmake-re (the portable package, its
binaries in `/usr/local/bin`) with `TIPI_DISTRO_MODE=none`, so it drives
the image's own cmake, ninja and compilers instead of provisioning tipi's
distro (~900 MB) on first use. Three uids may be the one running it,
depending on how a container is entered (vscode 1000 via tools/tc, tipi
1001, tipi-rbe 108); they share group `tipi`, which owns
`/usr/local/share/.tipi` (tipi's hard-wired state directory) and, in the
zig image, the prewarmed zig cache, so `tools/tc` adds that group to its
`-u 1000:1000`. cmake-re knows nothing about presets: pass the toolchain
file, and put `--build` first when building. The build directory becomes
a symlink into cmake-re's mirror of the checkout under `.tipi`, so `cd`
into it (or use `ctest --test-dir`) rather than reasoning about the
path. cmake-re writes a git note into the source checkout, so the uid
running it must own that checkout.

Each toolchain lives in its own folder because that folder is cmake-re's
environment unit: everything beside the toolchain file is copied into the
environment and hashed into its identity. For a containerized toolchain
the folder holds `<name>.cmake`, `<name>.pkr.js` (the image it builds in,
pinned to the content tag) and, where the toolchain includes
`../common.cmake`, a `<name>.layers.json` pulling that one file in. The
environment files are written by `tools/gen-environments.py`, which
shares tools/tc's preset-to-image table; rerun it after anything that
moves the content tag (the `docker/` tree, the msan script, the zig
wrappers, the image workflow), and `--check` says whether they are
stale. They are what `cmake-re --remote` needs to run the same build on
tipi's infrastructure instead of here.

```sh
tools/tc --shell linux-zigmusl-cxx23-static
cmake-re --host -S . -B build/cmake-re-zig -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-zigmusl-cxx23/linux-zigmusl-cxx23.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake-re --build build/cmake-re-zig --host -j 8
ctest --test-dir build/cmake-re-zig
```

The zigmusl toolchain files find the wrappers by name for the same
reason: cmake-re copies the toolchain file into its own environment
directory, where a path relative to the file resolves nowhere.

### SVE testing: one binary, every vector length

The aarch64 presets compile fixed-length SVE kernels (sve256/sve512 +
sve2_128 by default; `-DBLAKE3PP_SVE_ALL_VARIANTS=ON` adds sve128,
sve2_256, sve2_512) next to NEON, and runtime dispatch exact-matches the
CPU's vector length. qemu-user's `QEMU_CPU` env selects the emulated VL, so
the whole dispatch matrix runs from one build, with no per-VL presets:

```sh
tools/tc linux-arm64-gcc15-cxx23                    # -cpu max: VL=512 -> sve512
tools/tc linux-arm64-gcc15-cxx23 -- bash -c \
  'QEMU_CPU=max,sve-default-vector-length=32 ctest --test-dir build/linux-arm64-gcc15-cxx23'   # VL=256 -> sve256
# sve-default-vector-length is in BYTES: 16 -> sve2_128, 32 -> sve256,
# 64 -> sve512. QEMU_CPU=max,sve=off is the NEON-only regression; it is
# also the config that catches load-time SVE leaks: experimental::simd
# emits SVE static initializers that SIGILL on non-SVE CPUs.
```

## Building locally

```sh
docker buildx bake -f docker/docker-bake.hcl              # everything
docker buildx bake -f docker/docker-bake.hcl clang22      # one image
```

Version pins (Ubuntu digests, CMake, Ninja, zig, SDE) live at the top of
`docker-bake.hcl` and in the family Dockerfiles' ARG defaults.

## Gotchas

- **Host paths**: the devcontainer's docker CLI talks to the *host* daemon,
  so `tc`'s `-v` mounts must be host paths. devcontainer.json passes the
  workspace in as `LOCAL_WORKSPACE_FOLDER`; if you see an error about
  them, rebuild the devcontainer. A "clone in container volume" workspace has
  no host path and cannot use `tc`.
- **uid 1000**: images hard-code the `vscode` user at uid/gid 1000 (matching
  the devcontainer and this WSL host). A host with a different uid would need
  a `-u` override in `tc`.
- **GHCR auth**: images in a private repo need the *host* daemon logged in:
  `gh auth token | docker login ghcr.io -u pysco68 --password-stdin`.
- **Sanitizer runs** need `--cap-add=SYS_PTRACE --security-opt
  seccomp=unconfined`; `tc` passes these (same as devcontainer.json runArgs).
- **Intel SDE** (AVX-512 runs; qemu's TCG never implemented AVX-512) is baked
  into gcc16/clang22. Its license does not clearly permit redistribution,
  which is fine while the GHCR packages stay private; make them public and
  SDE must move to an on-demand download instead.
- **apt archive lifetimes**: the apt.llvm.org noble-20 pocket can be dropped
  when LLVM ages it out. If a
  pocket disappears, move that one image up a distro cohort in
  `docker-bake.hcl` (one line); the glibc floor rises accordingly.
