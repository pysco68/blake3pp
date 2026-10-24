# The static Linux binaries

Every release ships six fully static Linux archives, one per
architecture: x86_64, aarch64, riscv64, ppc64le, s390x and loongarch64.
A seventh target, mips64el, is built the same way in CI but not released: GCC
against a static glibc rather than zig against musl, equally standalone
and equally free of a glibc floor, but emulator-tested only (see
[Architectures and kernels](architectures.md#msa-has-never-run-on-hardware)). Each released archive
carries the tools and benchmarks (`blake3ppsum`, `blake3ppgen`,
`blake3pp_bench`, `blake3pp_bench_file`); the library itself is meant to
be built from source.

## What "fully static" means here

A conventional Linux binary is dynamically linked against the glibc it
was built with, and glibc's symbol versioning makes that a one-way
door: a binary built on a current distro refuses to start on an older
one, even when it uses nothing new. Linking glibc statically is not a
way out either; its name-service and `dlopen` machinery keep reaching
for shared objects at runtime. Linking musl statically is.

The release binaries therefore:

- link musl statically, so there is no libc version coupling at all
- contain no dynamic loader; the kernel maps one ELF file and jumps,
  and there is nothing to resolve at startup
- depend only on the Linux syscall ABI, so they run on any Linux kernel
  of the last decade
- bundle mimalloc as the tool allocator, because musl's own mallocng
  serializes allocation under threads, which would throttle the
  multi-core paths

The practical consequence is one file per architecture that runs on any
distro: current or ancient, glibc- or musl-based, or a `FROM scratch`
container.

## How: zig's toolchain as the cross compiler

The static presets
(`linux-{,arm64-,riscv64-,ppc64le-,s390x-,loongarch64-}zigmusl-cxx23-static`)
build with zig's bundled clang (`zig cc`) targeting `<arch>-linux-musl`.
zig ships musl headers and sources for every supported target, so a
single toolchain cross-builds all six architectures with no
per-architecture sysroots to assemble. The
toolchain files live in `cmake/toolchains/linux-*zigmusl*/` and
document the zig-specific plumbing (wrapper scripts, linker-probe
workarounds) in place.

### ppc64le: vector registers after a caught exception

LLVM's libunwind, which zig links on `linux-ppc64le-zigmusl-cxx23-static`
(and which any clang toolchain using `-unwindlib=libunwind` links), does
not restore the callee-saved vector registers v20-v31 when it resumes at a
`catch` on powerpc64 Linux:

- Its ppc64 `jumpto` restores them only for the bits set in VRSAVE, and
  the ELFv2 ABI leaves VRSAVE at 0, so none are restored.
- A value the compiler kept in one of them comes back as whatever the
  throwing code left there.

Seen in this project as a test that took the wrong branch after catching
a `system_error`, and as a hang where a counter kept in a vector register
lost an update on the exception path. libgcc's unwinder
(`linux-ppc64le-gcc15-cxx23`) restores them and is not affected.
Reproduced with zig 0.16 to 0.18-dev and with Debian's clang and
libunwind 21.1.8, under qemu-ppc64le. Reported upstream as
[llvm/llvm-project#231806](https://github.com/llvm/llvm-project/issues/231806).

**The programs this project builds carry a workaround.**
`tooling/vrsave_workaround.cpp` sets VRSAVE to all ones before `main()`;
threads created afterwards inherit it, and libunwind then restores every
vector register. It is compiled into the tools, the tests and the
benchmarks through `blake3pp::node_program`, and is empty on every other
target. `tests/unwinder.cpp` keeps the toolchain bug visible: one case
clears VRSAVE and is expected to fail on this toolchain, and another
checks that the workaround reaches a new thread. When the first case
reports that it should have failed, the toolchain is fixed, and both the
workaround and the expectation can go.

**The library does not apply it.** Changing a process-wide register
setting is the program's decision. A program that links blake3pp with
LLVM's libunwind on powerpc64 Linux and catches exceptions should do the
same in its own startup:

```cpp
#include <version>  // defines _LIBCPP_VERSION under libc++

#if defined(__powerpc64__) && defined(__linux__) && defined(_LIBCPP_VERSION)
[[gnu::constructor]] static void mark_vector_registers_live() {
  asm volatile("mtvrsave %0" ::"r"(-1));
}
#endif
```

## Why runtime SIMD dispatch survives static linking

The usual casualty of full static linking is runtime CPU dispatch,
because the common mechanism for it is GNU ifunc: a resolver function
that the dynamic loader runs while binding symbols. With no loader in
the picture, ifunc is fragile at best and broken at worst.

blake3pp never uses ifunc. Dispatch is a plain CPU probe plus a
function-pointer table selected at first use: ordinary C++ that behaves
identically in static and dynamic links. Every kernel variant the
architecture supports is compiled into the binary, and the best one the
running CPU can execute is picked at startup. The static archives keep
the full fat-binary behavior.

CPU detection is static-link-friendly by the same discipline:

- probes read HWCAP/auxv and use raw syscalls where needed; the riscv64
  `hwprobe` syscall is invoked directly because musl has no wrapper for
  it
- `__builtin_cpu_supports` is deliberately not used: it drags in
  libgcc/compiler-rt support machinery that is not reliably present in
  a static musl link

See `src/dispatch/cpu_detect_*.cpp` for the per-platform probes.

## Two compilers, one binary: riscv64 and s390x

zig's clang cannot produce two of the kernels the fat binary wants:

| archive | kernel | why zig/clang cannot build it | external compiler |
|---------|--------|-------------------------------|-------------------|
| riscv64 | `xthead` (draft RVV 0.7.1) | LLVM never merged the XTheadVector extension | `riscv64-linux-gnu-g++` |
| s390x   | `vxe` (z14 vector) | LLVM's SystemZ backend compiles the code but scalarizes the vector ops: about 10 vector instructions in the whole binary versus GCC's thousands, including the single-instruction `verllf` rotate | `s390x-linux-gnu-g++` |

The s390x case is the one no build check can catch. The clang-built
kernel is correct. It passes every test and reports itself as a vector variant,
while performing like the scalar one. No compile probe detects that, so
the build policy is explicit: under a non-GNU primary compiler, the vxe
kernel comes from GCC, or it is not registered at all.

For these two kernels the build uses the `EXTERNAL_COMPILER` mode of
`cmake/ArchKernels.cmake`. One translation unit is compiled by the distro
GCC cross compiler, which is in the zig toolchain image for exactly this
purpose, and its object file is linked into the same fat binary zig
produces.

Mixing two compilers' output is safe here for three reasons:

- the external object is compiled with `-fno-exceptions -fno-rtti`
  `-fno-stack-protector`, so it needs no compiler runtime support
- `nm` on the object shows exactly one undefined C++ symbol, the scalar
  kernel table it forwards its single-block entry points to, and no
  libc references beyond memcpy-class symbols every libc provides
- the kernel table crosses the compiler boundary as a plain struct of
  function pointers, which the Itanium C++ ABI defines identically for
  both compilers

The riscv64 archive carries the `xthead` kernel by default, which is
what lets a T-Head board (SG2042, C906/C910 class) run the draft-0.7.1
vector path from the standard release artifact.

## The loongarch64 archive is emulator-tested only

It is built and tested the way the other five are, and nothing has run
it on LoongArch silicon: the whole suite passes under qemu-loongarch64
at three capability levels, and that is the extent of it. See
[Architectures and kernels](architectures.md#lsx-and-lasx-have-never-run-on-hardware-either),
which also covers the one toolchain quirk the lane carries: zig's
loongarch64 baseline enables LSX, so the archive is compiled `-mno-lsx`
and the two kernels raise the vector unit for their own translation
unit.
