# Toolchain images

One Docker image per compiler, so CI jobs and local matrix runs pull a small
prebuilt image instead of the old 8.6 GB kitchen-sink devcontainer. All 26
`cmake/toolchains/*.cmake` files are unchanged: they name compilers by bare
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
| `toolchain-gcc12` | 22.04 | 2.35 | `linux-gcc12-*` | |
| `toolchain-gcc14` | 24.04 | 2.39 | `linux-gcc14-*` | |
| `toolchain-gcc16` | 26.04 | ~2.42 | `linux-gcc16-*` (incl. asan/tsan/coverage) | gdb, valgrind, lcov/gcovr, Intel SDE |
| `toolchain-clang18` | 24.04 | 2.39 | `linux-clang18-*` | g++-14 tree for the libstdcxx pin |
| `toolchain-clang20` | 24.04 | 2.39 | `linux-clang20-*` | g++-14 tree; clang from apt.llvm.org noble-20 |
| `toolchain-clang22` | 26.04 | ~2.42 | `linux-clang22-*` (incl. asan/tsan/msan/fuzzer/coverage/cxx2c) | g++-16 tree, libfuzzer/libomp, gdb, valgrind, Intel SDE, **/opt/libcxx-msan** |
| `toolchain-arm64-gcc15` | 26.04 | (aarch64 sysroot) | `linux-arm64-gcc15-*` | qemu-aarch64 |
| `toolchain-zig` | 26.04 | n/a (static musl) | `*zigmusl*`, `tools/make-release.sh` | qemu-aarch64, aarch64 strip, prewarmed zig cache |
| `toolchain-emscripten` | 26.04 | n/a (wasm) | `wasm32-*` | node; the apt package's frozen sysroot cache covers pthread/wasm-eh/simd |

Registry: `ghcr.io/pysco68/blake3pp/toolchain-<name>`, tags `latest` +
immutable `sha-<short>`. Built and pushed by
`.github/workflows/toolchains.yml` whenever `docker/**` changes.

## Daily use

From the (slim) devcontainer, via docker-outside-of-docker:

```sh
tools/tc linux-gcc12-cxx20                 # configure + build + ctest
tools/tc linux-clang22-cxx26-msan          # msan against the baked libc++
tools/tc linux-zigmusl-cxx23-static -- sh tools/make-release.sh
tools/tc --shell wasm32-emcc-cxx23
tools/tc --pull                            # refresh after a CI image push
```

`tc` mounts the workspace at `/workspaces/blake3pp` (the devcontainer's own
path), so build trees and
`compile_commands.json` are valid on both sides.

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
- **apt archive lifetimes**: jammy (gcc12) leaves standard support 2027-04;
  the apt.llvm.org noble-20 pocket can be dropped when LLVM ages it out. If a
  pocket disappears, move that one image up a distro cohort in
  `docker-bake.hcl` (one line); the glibc floor rises accordingly.
