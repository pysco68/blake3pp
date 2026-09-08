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
| `toolchain-arm64-gcc15` | 26.04 | (aarch64 sysroot) | `linux-arm64-gcc15-*` | qemu-aarch64, gcovr (the `-coverage` preset); the gcc/g++ drivers behind `reclient-gcc-driver`, which neutralises the one probe line reclient's dependency scanner cannot ask an aarch64 GCC (`__has_attribute(__arm_streaming)`, an SME keyword there) |
| `toolchain-riscv64-gcc15` | 26.04 | (riscv64 sysroot) | `linux-riscv64-gcc15-*` | qemu-riscv64 (RVV 1.0, `QEMU_CPU=max,vlen=<bits>`); opt-in Xuantie-qemu stage for XTheadVector (`--set riscv64-gcc15.args.WITH_XUANTIE_QEMU=1`, builds T-Head's fork in a pinned 22.04 stage; verify the 0.7.1 kernel with the freestanding `tests/xthead_verify.cpp` harness under `qemu-riscv64-xuantie -cpu c906fdv`; glibc binaries cannot run on the th CPU models, see the harness header) |
| `toolchain-ppc64le-gcc15` | 26.04 | (ppc64le sysroot) | `linux-ppc64le-gcc15-*` | qemu-ppc64le (`QEMU_CPU=power8/9/10`) |
| `toolchain-s390x-gcc15` | 26.04 | (s390x sysroot) | `linux-s390x-gcc15-*` | qemu-s390x (big-endian; `QEMU_CPU=max,vxeh=off` for the scalar fallback) |
| `toolchain-zig` | 26.04 | n/a (static musl) | `*zigmusl*` (x86_64, aarch64, riscv64, ppc64le, s390x), `tools/make-release.sh` | qemu-user, aarch64+riscv64 binutils strip, zig cache prewarmed for all five musl targets (world-writable; `musl-run <arch> <binary>` runs a static binary through qemu when foreign), the zig wrappers (docker/wrappers/zig) on PATH as `<triple>-clang`/`-clang++` |
| `toolchain-emscripten` | 26.04 | n/a (wasm) | `wasm32-*` | node; the apt package's frozen sysroot cache covers pthread/wasm-eh/simd; `wasm32-emscripten-clang`/`-clang++` wrap emcc/em++ under names reclient's dependency scanner accepts (the toolchain's Platform/Emscripten.cmake shim selects them) |

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
`/usr/local/share/.tipi` (tipi's hard-wired state directory), so
`tools/tc` adds that group to its `-u 1000:1000`; the zig image's
prewarmed cache is world-writable instead, since a remote worker runs
the wrapper as whatever uid it likes. cmake-re knows nothing about
presets: pass the toolchain file, put `--build` first when building, and
pass `-DCMAKE_BUILD_TYPE` explicitly (cmake-re configures Debug when
none is given, where plain cmake would leave the toolchain's default). The build directory becomes
a symlink into cmake-re's mirror of the checkout under `.tipi`, so `cd`
into it (or use `ctest --test-dir`) rather than reasoning about the
path. cmake-re writes a git note into the source checkout, so the uid
running it must own that checkout.

In CI the same build runs through cmake-re when the repository variable
`BLAKE3PP_CI_DRIVER` is `cmake-re` (or the `driver` dispatch input says
so): every Linux container lane of ci.yml then calls tools/ci-test.sh
with `BLAKE3PP_CMAKE_RE=1`, which configures and builds through
`cmake-re --host` (the image's own compilers) or, when the variable
`BLAKE3PP_CMAKE_RE_MODE` is `distributed`, `cmake-re --host
--distributed` (compiles on the EngFlow cluster, the remote action cache
serving repeats), tests where the build lands, and uploads the same
artifacts as the plain-cmake run. The cmake-re-only preparation is one
composite action, .github/actions/setup-cmake-re: for the distributed
mode the EngFlow credentials from the repository secrets (`RBE_SERVICE`,
`RBE_TLS_CLIENT_AUTH_KEY`, `RBE_TLS_CLIENT_AUTH_CERT`), the environment
files retargeted at the registry the cluster can pull from (the variable
`BLAKE3PP_RBE_ENV_REGISTRY`, the Docker Hub mirror until GHCR is public),
and cmake-re's working directory (`cmake-re --info json`,
`tipi_workdir`: the mirrored source, its build tree, the hfc dependency
builds) restored from the actions cache per lane, image tag and cmake-re
distro (.github/actions/cache-tipi-mirror), so a rerun configures in
seconds and builds only what changed; the version stamp looks through
cmake-re's sync commit so that an unchanged tree stays a no-op. Two
settings ride along with every cmake-re run:
`TIPI_DISABLE_AR_RANLIB_DRIVER=ON`, because tipi's ranlib action rewrites
its input archive in place, which the remote sandbox denies, and a `USER`
for the dependency scanner. The zig wrappers answer that scanner's probes
in clang's format (see docker/wrappers/zig), which is what lets the musl
lanes distribute at all.

The base recipe also carries a docker client (the static tarball) and
openssh-server for cmake-re's other modes. With the host's docker socket
in the container (`-v /var/run/docker.sock:/var/run/docker.sock`; a
GitHub job container has it by default), cmake-re resolves the
environment's image on the daemon, pulls it if it is not there, writes
`<name>.container.lock` beside the toolchain (the registry digest it
resolved to; gitignored, regenerated on demand) and, for the local
containerized mode, starts the environment container running sshd and
builds through it. A job container's own image is already on the
runner's daemon, so `cmake-re --remote` works from inside it with
nothing installed on the runner.

The cluster behind `cmake-re --distributed` pulls the environment images
itself, and GHCR is private, so toolchains.yml copies every image it
builds to a public mirror: the repository variable
`BLAKE3PP_RBE_ENV_REGISTRY` names it (e.g. `docker.io/<namespace>`), the
secrets `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` log in, and each image
lands there as `blake3pp-toolchain-<image>` at the same content tag (a
manifest copy, no rebuild), which is the spelling
`BLAKE3PP_TC_IMAGE_TEMPLATE` gives `gen-environments.py` for the
environment files.

Each toolchain lives in its own folder because that folder is cmake-re's
environment unit: everything beside the toolchain file is copied into the
environment and hashed into its identity. For a containerized toolchain
the folder holds `<name>.cmake`, `<name>.pkr.js` (the image it builds in,
pinned to the content tag) and, where the toolchain includes
`../common.cmake`, a `<name>.layers.json` pulling that one file in. The
environment files are written by `tools/gen-environments.py`, which
shares tools/tc's preset-to-image table; `--check` says whether they
are stale against the content tag. ci writes the tag it runs with into
them before building (a dirty checkout, which cmake-re mirrors as such)
and, on `main`, proposes the committed update as one self-updating pull
request, so a moved tag never blocks a run. They are what
`cmake-re --remote` needs to run the same build on tipi's
infrastructure instead of here.

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
directory, where a path relative to the file resolves nowhere. The
wrappers are named `<triple>-clang` and `<triple>-clang++` because
reclient's dependency scanner classifies a compiler by its basename and
aborts on anything it does not recognise, and they choose zig's cache
directory themselves because the scanner (and a remote executor) runs
compilers under a scrubbed environment where zig cannot derive one.

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

## Wrappers

Every script an image puts in front of a compiler lives in
`docker/wrappers/<image>/` and is COPY'd to `/usr/local/bin` (or over the
versioned driver, for arm64-gcc15) by that image's recipe, so the recipe,
the wrapper and its reason sit together and the content tag covers them:

| directory | scripts | why |
|---|---|---|
| `wrappers/zig/` | `<triple>-linux-musl-clang`, `-clang++` (five targets), `musl-run` | zig under names reclient's dependency scanner accepts, answering its probes in clang's format; `musl-run` executes a static musl binary through qemu when foreign |
| `wrappers/arm64-gcc15/` | `reclient-gcc-driver` | installed over `aarch64-linux-gnu-gcc-15`/`g++-15`; neutralises the one scanner probe line an aarch64 GCC rejects (`__has_attribute(__arm_streaming)`) |
| `wrappers/emscripten/` | `wasm32-emscripten-clang`, `-clang++`, `emscripten-link-launcher` | emcc/em++ under accepted names; the launcher declares the `.wasm` sidecar of a remote link (`RBE_output_files`) so it comes back with the `.js` |

The toolchain files name the wrappers bare (they are on PATH in the
image; the zig ones are also found in a checkout by the `HINTS` path), and
the wasm toolchain's `Platform/Emscripten.cmake` shim selects the
emscripten ones only when they exist, so the presets still configure on a
machine without them.

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
