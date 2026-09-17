# Architectures and kernels

[← blake3pp](../README.md)

Every SIMD kernel your target supports is compiled into the binary, and
the one that runs is chosen when the program starts. You do not pick, and
you do not ship a build per processor.

This page lists what each architecture gets, and covers the two targets
where that story does not hold: MIPS, which no one has run on hardware,
and wasm, where a fat binary is not possible at all.

## What a build carries

| architecture  | kernels |
|---------------|---------|
| x86-64        | SSE4.2, AVX2, AVX-512 |
| ARM (aarch64) | NEON; fixed-length SVE 256/512 and SVE2 128 (more VLs opt-in) |
| RISC-V        | RVV 1.0 at VLEN 128/256/512, each with a Zvbb-rotate twin; opt-in T-Head draft-0.7.1 XTheadVector |
| POWER         | VSX |
| IBM z         | VXE (z14+, **big-endian**) |
| MIPS          | MSA (MIPS32r5/MIPS64r5+) — **emulator-tested only**, see below |
| wasm          | SIMD128 (see [below](#the-wasm-build)) |

Every build also carries a scalar kernel as a fallback.

## SVE is pinned to one vector length

The SVE kernels are vector-length-specific. Dispatch selects one only
when the CPU's runtime vector length equals the length that kernel was
compiled for, because that is the only case the ABI guarantees.

Where that length comes from depends on the platform:

- On Linux, from `prctl`.
- On Windows, from `rdvl`. The platform reports whether SVE is present
  but not how wide it is.

Building them needs a compiler that accepts a fixed vector length.
`clang-cl` accepts one, in its cc1 spelling; `cl` does not. An arm64
Windows build therefore carries SVE kernels only when clang-cl built it.

## MSA has never run on hardware

The MSA kernel carries an asterisk the others do not. Under qemu it is
correct: it matches the official test vectors byte for byte against the
x86-64 result, and its dispatch is validated in both directions, with an
emulated core without MSA falling back to scalar and one with it
selecting the kernel.

No MSA silicon has ever run it, and none is reachable. It carries no
throughput number, and it should be treated as untested on hardware. CI
builds and tests it; no release archive contains it.

Two details follow from having no hardware and no other toolchain:

- GCC builds it, through the vector-extension provider. No std provider
  deduces a vector width on this target, xsimd has no MSA backend, and
  clang emits no MSA from the same source at all.
- Its static build links a static glibc, where the released archives
  link a static musl. Both are equally standalone: no interpreter, no
  dynamic section, and no `GLIBC_` version symbol, so this one carries no
  glibc floor either. A static glibc gives up NSS, which this tool never
  asks for.

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
