# The static Linux binaries

Every release ships five fully static Linux archives, one per
architecture: x86_64, aarch64, riscv64, ppc64le and s390x. Each archive
carries the tools and benchmarks (`blake3ppsum`, `blake3ppgen`,
`blake3pp_bench`, `blake3pp_bench_file`); the library itself is meant to
be built from source. This page explains what "fully static" buys, how
the build produces it, why runtime SIMD dispatch survives it, and why
two of the archives are the work of two compilers at once.

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

The static presets (`linux-{,arm64-,riscv64-,ppc64le-,s390x-}zigmusl-`
`cxx23-static`) build with zig's bundled clang (`zig cc`) targeting
`<arch>-linux-musl`. zig ships musl headers and sources for every
supported target, so a single toolchain cross-builds all five
architectures with no per-architecture sysroots to assemble. The
toolchain files live in `cmake/toolchains/linux-*zigmusl*.cmake` and
document the zig-specific plumbing (wrapper scripts, linker-probe
workarounds) in place.

## Why runtime SIMD dispatch survives static linking

The usual casualty of full static linking is runtime CPU dispatch,
because the common mechanism for it is GNU ifunc: a resolver function
that the dynamic loader runs while binding symbols. With no loader in
the picture, ifunc is at best fragile and at worst simply does not
work.

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

The s390x case is the treacherous one: the clang-built kernel is
correct, passes every test, and reports itself as a vector variant
while performing like the scalar one. A compile probe cannot detect
that, so the build policy is explicit: under a non-GNU primary compiler
the vxe kernel comes from GCC or is not registered at all.

For these two kernels the build uses the `EXTERNAL_COMPILER` mode of
`cmake/ArchKernels.cmake`: the one translation unit is compiled by the
distro GCC cross compiler (present in the zig toolchain image for
exactly this purpose) and its object file is linked into the same fat
binary zig produces. Here's why this can work in these special circumstances:

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
