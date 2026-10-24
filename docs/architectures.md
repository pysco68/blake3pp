# Architectures and kernels

[← blake3pp](../README.md)

Every SIMD kernel your target supports is compiled into the binary, and
the one that runs is chosen when the program starts. You do not pick, and
you do not ship a build per processor.

This page lists what each architecture gets, down to the runtime check and
the test that covers it; the targets where that story does not hold
(MIPS and LoongArch, which no one has run on hardware, and wasm, where a
fat binary is not possible at all); and, at the end, [the instruction
sets that have no kernel](#what-has-no-kernel-and-what-it-would-take)
and what each one would require.

## What a build carries

| architecture  | kernels |
|---------------|---------|
| x86-64        | SSE4.2, AVX2, AVX-512 |
| ARM (aarch64) | NEON; fixed-length SVE 256/512 and SVE2 128 (more VLs opt-in) |
| RISC-V        | RVV 1.0 at VLEN 128/256/512, each with a Zvbb-rotate twin; opt-in T-Head draft-0.7.1 XTheadVector |
| POWER         | VSX |
| IBM z         | VXE (z14+, **big-endian**) |
| MIPS          | MSA (MIPS32r5/MIPS64r5+) — **emulator-tested only**, see below |
| LoongArch     | LSX (128-bit), LASX (256-bit) — **emulator-tested only**, see below |
| wasm          | SIMD128 (see [below](#the-wasm-build)) |

Every build also carries a scalar kernel as a fallback.

## Every variant, exactly

One row per kernel variant: what the CPU must have for dispatch to select
it, how wide it runs, which SIMD facade compiles it, what the runtime
check reads, and what has actually executed it.

| variant | needs | lanes | facade | runtime check | how it is tested |
|---|---|---|---|---|---|
| `scalar` | nothing | 1 | forced scalar | always available, ranked last | every lane; it is also the oracle the wide kernels are compared against |
| `sse42` | SSE4.2 | 4 | project provider | `cpuid(1).ecx[20]` | native, on the x86 Linux and Windows x64 runners |
| `avx2` | AVX2 and OS YMM state | 8 | project provider | `cpuid(7,0).ebx[5]` + `XCR0` | native, same runners |
| `avx512` | F, CD, VL, BW, DQ and OS ZMM state | 16 | project provider | five `cpuid(7,0).ebx` bits + `XCR0` | native where the runner's silicon has it; Intel SDE `-skx` on the Linux x86 lanes that do not. Not under SDE on the sanitizer lanes, and not on Windows |
| `neon` | nothing: mandatory on AArch64 | 4 | project provider | none; presence of the kernel is the answer | native on Apple silicon and on arm64 Windows; the qemu `sve=off` pass; Neoverse N2. Pure `cl` builds none and ships scalar |
| `sve256` | SVE, runtime VL exactly 256 | 8 | xsimd (pinned) | `HWCAP_SVE` + `prctl(PR_SVE_GET_VL)` on Linux; the same auxv bit through `elf_aux_info` + `rdvl` on FreeBSD; `IsProcessorFeaturePresent` + `rdvl` on Windows | qemu-user at VL=256; no hardware |
| `sve512` | SVE, VL exactly 512 | 16 | xsimd (pinned) | as above | qemu-user at VL=512, the default on every arm64 lane; no hardware |
| `sve2_128` | SVE2, VL exactly 128 | 4 | xsimd (pinned) | as above plus `HWCAP2_SVE2` | qemu-user at VL=128; a FreeBSD 15.1 guest under qemu-system-aarch64 at `sve-max-vq=1`, every push; real silicon, Neoverse N2 in CI and Neoverse V2 in the benchmarks |
| `sve128`, `sve2_256`, `sve2_512` | the same, at the other lengths | 4, 8, 16 | xsimd (pinned) | as above | opt-in build (`BLAKE3PP_SVE_ALL_VARIANTS`), not in CI. No shipping silicon has these combinations, so an emulator is the only thing that ever selects them. Each falls back gracefully to the same-length SVE1 kernel, which is why the set can wait for hardware: an SVE2 part at 256 selects `sve256` and loses only the XAR fused rotate. Fujitsu's Monaka (Armv9.3, reported as 256-bit SVE2, 2027) is the first announced part that would want `sve2_256`; `sve128` would need an SVE1-only core at the minimum length, which has never shipped |
| `rvv128`, `rvv256`, `rvv512` | RVV 1.0, `vlenb` exactly 16/32/64 | 4, 8, 16 | xsimd (pinned) | hwprobe `IMA_EXT_0` V bit, then `vlenb` | the qemu VLEN matrix, though only at VLEN 128 (the `zvbb=false` pass; the wider qemu configurations have Zvbb on, so they select the twins); `rvv256` on real silicon, a SpacemiT K1 board |
| `rvv128_zvbb`, `rvv256_zvbb`, `rvv512_zvbb` | the same plus Zvbb | 4, 8, 16 | xsimd (pinned) | hwprobe `ZVBB` (no HWCAP letter exists) + `vlenb` | qemu-user at VLEN 128 (the default), 256 and 512; no Zvbb silicon has run them |
| `xthead` | XTheadVector, the draft RVV 0.7.1 encoding | 4 | none: hand-written intrinsics, sizeless types cannot back the facade | hwprobe vendor key (Linux 6.13+), else the trap-guarded probes behind `run_trap_probes()`, else `BLAKE3PP_ASSUME_XTHEADVECTOR=1` | opt-in build (`BLAKE3PP_XTHEAD_KERNEL`, on in the riscv64 musl preset). Mainline qemu cannot execute `th.*` at all; T-Head's qemu fork runs the freestanding verifier, and an SG2042 board runs it on 0.7.1 silicon |
| `vsx` | VSX | 4 | xsimd (pinned) | `PPC_FEATURE_HAS_VSX` | qemu-user at POWER9 and POWER10; no hardware. Built at `power9`, the first flag spelling that probed clean |
| `vxe` | z14 vector-enhancements-1 | 4 | xsimd (pinned), compiled by GCC — an external object when the primary compiler is not GCC | `HWCAP_S390_VXRS` and `HWCAP_S390_VXRS_EXT`, both required | qemu-user at `max` and at `vxeh=off` for the fallback; no hardware. The only big-endian variant: every word load and store byteswaps |
| `msa` | MSA (MIPS32r5/MIPS64r5+) | 4 | GNU vector extensions at 16 bytes | `HWCAP_MIPS_MSA` | built and instruction-audited in CI, and the refusal path runs there; executing the kernel is a local qemu run, see below |
| `lsx` | LoongArch LSX | 4 | GNU vector extensions at 16 bytes | `HWCAP_LOONGARCH_LSX` | qemu-loongarch64 at `max,lasx=off`; no hardware |
| `lasx` | LoongArch LASX | 8 | GNU vector extensions at 32 bytes | `HWCAP_LOONGARCH_LASX` | qemu-loongarch64 at the default model, which is `la464`; no hardware |
| `simd128` | wasm SIMD128 | 4 | project provider | reported unconditionally, see [below](#the-wasm-build) | node, in the emscripten lane |

**Lanes** are 32-bit lanes, which is how many inputs the kernel hashes at
once. `BLAKE3PP_MAX_SIMD_DEGREE` accepts 1, 2, 4, 8 or 16, so 16 lanes —
a 512-bit register — is the widest kernel the tree can hold.

**Facade.** The project provider is `std::simd` where the standard library
has a usable one, else `<experimental/simd>`, else xsimd;
`BLAKE3PP_SIMD_PROVIDER` overrides the choice. A variant that pins one
does so because the generic answer is wrong there: libstdc++'s
`experimental::simd` SVE backend emits SVE in load-time initializers,
which SIGILLs on non-SVE hardware and defeats the fat binary; no std
provider has an RVV, MSA, LSX or LASX width at all; and GCC 16's
`<simd>` deduces its native width from an x86-only traits table, so on
every other target
`std::simd::vec<uint32_t>` is one lane wide. The kernel audit's
require-rule (`tools/kernel-audit.json`) is what catches a variant that
came out scalar under a facade that could not reach its ISA.

**Flags are probed, never assumed.** Each family's registrar tries the
candidate spellings of its `-m`/`-march` flags at configure time and
registers only what compiled (`cmake/KernelVariants.cmake`). A toolchain
that cannot express a variant produces a binary without it, not a broken
one.

### Which emulator configurations CI actually runs

`tools/ci-test.sh` holds one `QEMU_CPU` matrix per architecture — vector
lengths, and the feature-off configurations that make dispatch fall back.
It runs the full matrix only on the presets whose binaries ship (the
static musl ones) and the coverage presets, where each configuration
contributes different lines. Every other cross lane runs the default
configuration only: one VL, one VLEN, one CPU model.

That default pass runs with `QEMU_CPU` unset, which gives VL=512 on
aarch64, VLEN=128 with V and Zvbb on riscv64, z14 vectors on s390x, and a
POWER model new enough for the distribution's power9 userland on ppc64le,
and `la464` with both vector units on loongarch64 — and, on mips64el, an
r2-era core with no MSA at all.

### What silicon has run a kernel

x86-64 on the hosted runners, and in the published benchmarks Zen 5,
Sapphire Rapids and Zen 3+. Apple silicon (the benchmarked one is an M2).
Neoverse N2 in CI and Neoverse V2 in the benchmarks. arm64 Windows. Three
RISC-V boards: a JH7110 with no vector unit at all, a SpacemiT K1 for
`rvv256`, and an SG2042 for `xthead`. The arm64, Windows-on-arm and
RISC-V board lanes are gated on repository variables, so an arbitrary CI
run may contain none of them.

POWER, IBM z, MIPS and LoongArch have no hardware lane at all. Their
kernels are correct against the official vectors under qemu and carry no
throughput number.

## SVE is pinned to one vector length

The SVE kernels are vector-length-specific. Dispatch selects one only
when the CPU's runtime vector length equals the length that kernel was
compiled for, because that is the only case the ABI guarantees.

Where that length comes from depends on the platform:

- On Linux, from `prctl`.
- On Windows, from `rdvl`. The platform reports whether SVE is present
  but not how wide it is.
- On FreeBSD, from `rdvl` for the same reason. Presence comes from the
  same `HWCAP_SVE` bit Linux publishes -- the bit positions are Arm's --
  read with `elf_aux_info`, which answers a status code rather than a
  value and so cannot report absence as a zero. The bit is set only by a
  kernel that saves SVE state across a context switch, which FreeBSD
  gained in 15.0: on 14.x the probe correctly finds no SVE and dispatch
  takes NEON. Verified on a FreeBSD 15.1 arm64 guest under
  qemu-system-aarch64, whose `-cpu max,sve-max-vq=N` makes the vector
  length a dial: at 512, 256 and 128 bits dispatch selects `sve512`,
  `sve256` and `sve2_128` and rejects the other two lengths, and with
  `sve=off` it takes NEON -- each with the whole suite passing. CI now
  runs that guest on every push (`freebsd-arm64-test`), pinned to one
  length rather than sweeping all four: `sve-max-vq=1`, a 128-bit
  vector, because that is what essentially all shipping Armv9 silicon
  has, and the lane asserts that dispatch engaged an SVE2 kernel rather
  than merely that the suite passed.

Building them needs a compiler that accepts a fixed vector length.
`clang-cl` accepts one, in its cc1 spelling; `cl` does not. An arm64
Windows build therefore carries SVE kernels only when clang-cl built it.

## MSA has never run on hardware

The MSA kernel carries an asterisk the others do not. Under qemu it is
correct: it matches the official test vectors byte for byte against the
x86-64 result, and dispatch is validated in both directions: an emulated
core without MSA falls back to scalar, one with it selects the kernel.

The two directions do not run in the same place. CI builds the kernel,
audits its object for MSA instructions and runs the refusal direction on
every change, because qemu's default mips64el core is r2-era and has no
MSA. Selecting the kernel needs `QEMU_CPU=Loongson-3A4000`, a pass
`tools/ci-test.sh` runs only on static-musl and coverage presets; no mips
preset is either, since zig builds no mips target. That one is a local
run.

No MSA silicon has ever run it, and none is reachable. It carries no
throughput number, and it should be treated as untested on hardware. No
release archive contains it.

Two details follow from having no hardware and no other toolchain:

- GCC builds it, through the vector-extension provider. No std provider
  deduces a vector width on this target — libstdc++ measures
  `native_simd<uint32_t>` at one lane here, because its ABI list has
  never heard of the ISA — and xsimd has no MSA backend. A clang build
  registers no MSA kernel either: the feature probe asks for
  `__mips_msa_width`, which clang does not define, so the variant is
  declined rather than mislabelled. Only GCC has ever built it.
- Its static build links a static glibc, where the released archives
  link a static musl. Both are equally standalone: no interpreter, no
  dynamic section, and no `GLIBC_` version symbol, so this one carries no
  glibc floor either. A static glibc gives up NSS, which this tool never
  asks for.

## LSX and LASX have never run on hardware either

The two LoongArch kernels carry MSA's asterisk. Under qemu-loongarch64
they match the official test vectors byte for byte against the x86-64
result, and the dispatch ladder is exercised at every capability level
the emulator can present: the default `la464` carries both vector units
and selects `lasx`, `max,lasx=off` selects `lsx`, and
`max,lsx=off,lasx=off` falls back to scalar. CI runs all three on every
change, on the static musl preset whose binaries ship.

No LoongArch silicon has run them, and none is reachable. They carry no
throughput number.

Two details follow from the toolchain, which is zig's clang and musl:

- zig's loongarch64 baseline carries `+lsx`, where its riscv64 baseline
  is plain `rv64gc` with no vector unit. Left alone it would put 128-bit
  vector code in every translation unit, the scalar kernel and the
  capability probe included, and the binary would fault on an LA64 part
  without LSX. The toolchain file compiles everything `-mno-lsx`; the two
  kernels raise the unit for their own TU.
- Raising it takes the cc1 spelling, `-Xclang -target-feature -Xclang
  +lsx`, which the registrar probes for first. Under this driver a later
  `-mlsx` does not undo `-mno-lsx`, and `-mlasx` after `-mno-lsx`
  produces a TU that predefines `__loongarch_asx` and
  `__loongarch_simd_width` 256 and generates no vector instruction at
  all. A feature-macro probe passes there; the kernel audit does not.

## The wasm build

wasm32-simd128 is the one target where the fat-binary premise cannot
hold, and the reason is how WebAssembly validates modules.

On a native ISA, an unsupported instruction is harmless as long as it
never executes. That is exactly what lets one binary carry AVX-512 next
to SSE4.2 and choose between them at runtime. A wasm engine instead
validates the whole module when it loads. An engine without SIMD128
rejects a module containing any SIMD opcode before a single instruction
runs, whether those opcodes would have executed or not.

So there is nothing to select between inside one module. Code that is
running has already passed validation, which means SIMD128 is available:
`available_arches()` reports it unconditionally there. The scalar kernel
is still compiled in and can be pinned for comparison runs, but it is not
a fallback, because an engine without SIMD128 never gets far enough to
reach it.

Degrading gracefully on wasm means shipping two modules and picking one
at load time, on the embedder's side. The standard test is to feed
`WebAssembly.validate()` a small SIMD probe module. Every current
mainstream engine supports SIMD128, and node has had it on by default
since 16, which is why the release ships the simd128 build alone.
`BLAKE3PP_WASM_SIMD128=OFF`, the `wasm32-emcc-cxx23-scalar` preset,
builds the other module.

### Two kinds of executable

The tools, tests and benches are node programs. `blake3pp::node_program`
gives them host files through NODERAWFS, a prespawned worker pool, and an
exit when `main` returns.

The browser module is `web/blake3pp-web-<kernels>.mjs` plus its `.wasm`:
the library's types behind embind, under their own names, as one ES
module usable from a page, a worker or node. Bytes are passed as an
address and a length into the module's memory, digests come back as hex,
and a scheduler is a `thread_pool` object the caller owns and passes,
exactly as C++ code passes a scheduler.

```js
import createBlake3pp from './blake3pp-web-simd128.mjs';
const b3pp = await createBlake3pp();               // spawns a worker per hardware thread
const buf = b3pp._malloc(len);
b3pp.HEAPU8.set(myBytes, buf);                     // bytes live in the module's memory
b3pp.hash(buf, len, 'auto');                       // hex digest; the variant by name
const pool = new b3pp.thread_pool(navigator.hardwareConcurrency);
b3pp.hash(buf, len, pool, 'auto');                 // the multi-core one-shot
const h = new b3pp.parallel_hasher(pool, 'auto');  // or new b3pp.hasher('auto')
h.update(buf, len); h.update('text'); h.finalize(); h.reset();
const r = new b3pp.hasher('auto').finalize_xof();  // output_reader: fill, seek, position
h.delete(); pool.delete(); b3pp._free(buf);        // embind objects are freed explicitly
```

### What a page has to arrange

A pooled call blocks the thread the module was instantiated on until the
pool is done. A browser forbids waiting on its main thread, so a page
instantiates the module in a Web Worker. wasm threads run on a
SharedArrayBuffer, which needs cross-origin isolation:
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`.

`b3pp.version()`, `b3pp.simd_provider()`, `b3pp.execution_provider()`,
`b3pp.compiled_arches()`, `b3pp.available_arches()` and
`b3pp.best_available()` report what `blake3ppsum --version` prints.

Only plain mode is bound; keyed and derive_key are not.

## What has no kernel, and what it would take

A new variant needs four things: a facade that reaches the ISA at a width
the build can name, a capability check the library can read *without*
executing the ISA, a compiler that accepts the flags, and something that
can run the result. The last one decides whether a variant ships enabled: the SVE variants
matching no silicon are opt-in, and MSA ships labelled as never having run
on hardware.

Three of the four are cheaper than they look, because the facade has a
provider that needs nothing from a SIMD library: GNU vector extensions at
a width the build states (`BLAKE3PP_VEXT_BYTES`), which is how MSA got a
kernel. The question for a new ISA is therefore not "does xsimd support
it" but "does the compiler lower a `vector_size` vector to it". The table
below answers that for the candidates, as a codegen probe of clang 22 on
one expression in the kernel's shape (`(a+b) ^ ((a<<7)|(b>>25))` over
`unsigned`). These are probes, not kernels: they say the compiler can
express the type, and nothing about correctness, speed, or the rest of the
kernel compiling.

| target | vector size | clang 22 emits |
|---|---|---|
| `mips64el` / `mipsisa64r6el` `-mmsa` | 16 B | MSA (`addv.w`, `xor.v`) |
| `armv8.1-m.main+mve` | 16 B | MVE (`vadd.i32 q0`, `veor`) |
| `aarch64_be` | 16 B | NEON, plus a `rev64`+`ext` pair around every memory access |
| `hexagon` `-mhvx` | 128 B | HVX (`v31.w = vadd(...)`, `vsplat`) |
| `hexagon` `-mhvx` | 16 B | no HVX: the scalar core's 64-bit pair ops |
| `ve` (SX-Aurora) | 16 B | VE vector instructions (`lvl`, `vldl.zx`) |
| `sparcv9` `-mvis3` | 8 B and 16 B | no vector instructions at all |

What xsimd 14.3 has, for comparison: the SSE/AVX/AVX-512 family, NEON,
fixed-length SVE, fixed-VLEN RVV, VSX, VXE and wasm. Nothing else, and no
`emulated` backend worth using for a kernel. libstdc++'s
`<experimental/simd>` has x86, NEON, SVE and PPC backends. GCC 16's
`<simd>` has an x86 traits table and a scalar fallback for every other
target, so "std::simd on a target GCC has `-march` support for" does not
follow from the ISA being supported by GCC: the standard library has to
know the ISA's width too, and today only x86 does.

| candidate | facade | detection | toolchain and emulator | verdict |
|---|---|---|---|---|
| More MIPS flavours (MIPS32r5, big-endian `mips64`) | already there: the vector-extension provider and the existing `msa` variant | already there: `HWCAP_MIPS_MSA`, and the registrar already matches `mips`/`mipsel`/`mips64` | one cross image per flavour; qemu-user covers all of them | Open and cheap. A big-endian MIPS build would be the second big-endian target, and the kernel's byteswapping path is already proven by `vxe`. No hardware either way, so it would join MSA's asterisk rather than escape it. |
| `mipsisa64r6el` | the probe above shows clang reaching MSA under the r6 target | unchanged | r6 is its own, non-backwards-compatible ISA: it needs its own sysroot and cross GCC, and a qemu CPU model that reports MSA. Which models do is unverified | Open, more work than the r5 flavours for the same zero hardware. |
| `aarch64_be` | NEON through the facade, at the cost of a lane-order fixup per memory access (probe above) | unchanged: the same auxv and `prctl` path | GCC and LLVM both target it; no mainstream distribution ships an `aarch64_be` sysroot, so the cross toolchain would have to be built. qemu-user has the target | Open in principle, unattractive in practice: a self-built toolchain, a kernel whose loads pay `rev64`, and no reachable hardware. |
| SVE and RVV above 512 bits | xsimd handles the types; the ceiling is this tree's | the exact-match checks already generalise: one more `vlenb`/VL value each | qemu can present VL and VLEN above 512 today | Open, and the one gap `arch.def` already names. 1024 bits is 32 lanes, above the 16 that `BLAKE3PP_MAX_SIMD_DEGREE` accepts, so it is new work in the kernel and in the subtree buffers, not a new row in a loop. No silicon above 512 bits exists to measure the result on, which is also why dispatch currently lands such a machine on scalar. |
| Hexagon HVX | reachable, at 128-byte vectors (probe above) | nothing to read: HVX is not a feature of a host CPU a Linux process runs on | LLVM targets Hexagon and qemu-user has a Hexagon target with HVX | The interesting experiment that does not fit the library's model; see below. |
| Arm Helium (MVE) | reachable, 128-bit, 4 lanes (probe above) | none, and none needed: an M-profile binary is built for its core | compilers yes; testing needs a system emulator running a board, not qemu-user | Open as a *freestanding* target, not as a dispatch target; see below. |
| Arm SME / SME2 | no backend anywhere, and ZA tiles do not fit the facade's register model: it would be a hand-written kernel, like `xthead` | both Linux and Windows report SME presence through the same surfaces the SVE probe already uses, and the Windows SDK's SME feature ids arrived alongside the SVE ones the probe spells out | compilers yes; qemu-user emulates SME; recent phone cores have it, no CI machine does | Open as an experiment with an honest question in front of it: whether an outer-product unit helps a hash whose state is 16 words wide. Nothing here is measured. |
| Loongson MMI | 64 bits is 2 lanes, the narrowest the facade can usefully carry; clang has no MMI support, so GCC's `-mloongson-mmi` would be the only route and whether it lowers an 8-byte vector is unverified | no HWCAP bit for it that the tree knows of | a Loongson MIPS sysroot; qemu's MMI coverage is unverified | Low reward: every Loongson part new enough to care about has MSA, which already has a kernel at twice the width. |
| SPARC VIS 3 | none. No SIMD library backend, and clang emits no vector instruction from the facade's type at 8 or 16 bytes (probe above) | no route the tree knows | Debian's sparc64 port is unofficial; qemu-sparc64 exists, but its VIS coverage is unverified and VIS3 unlikely | Closed. The facade has no way in, so a kernel here would be hand-written like `xthead`, for 64-bit-wide registers, on T4/M7-class hardware, none of which is reachable. |
| NEC SX-Aurora | LLVM's VE backend does lower the facade's type to VE vector instructions (probe above), but a 256-element, 64-bit-lane vector engine is not a 4-to-16-lane kernel | not applicable: the engine is a PCIe device with its own runtime, not a CPU feature | no emulator exists at all, and the card is the only way to run anything | Closed: no way to test it, and the shape of the machine wants a different kernel, not a wider one. |

Where a row above names a compiler, a distribution or an emulator this
tree has never used, that is knowledge about those projects rather than a
result from this one. The codegen probes are the only part of this section
that was run here, and nothing in it has been measured.

### Hexagon, and why a DSP is not a wide CPU

HVX is reachable through the facade and qemu can run it, so for once
testing is not the blocker. Two other things are. A 128-byte vector is 32
lanes, above the 16-lane ceiling the kernel is written to. And an HVX unit
is a DSP addressed through a remoting interface, not an instruction set the
calling process can discover and start using, so there is nothing for
runtime dispatch to select: a Hexagon build would be a separate artifact
with one kernel, the shape the wasm build already has.

### Helium, and the freestanding path

MVE is the one candidate where the fat binary is beside the point.
M-profile cores run no hosted OS, so there is no auxv to read and no
second variant to choose between; the core is known at build time. That
makes it a question for the [freestanding build](freestanding.md) rather
than for dispatch, and the testing shape is a system emulator running a
board image — something the project does not currently do anywhere.
