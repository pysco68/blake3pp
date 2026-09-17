# Architectures and kernels

[← blake3pp](../README.md)

## Architectures and kernels

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

The SVE kernels are vector-length-specific: dispatch selects one only when the CPU's runtime 
vector length equals the length the kernel was compiled for, since that is the only case the 
ABI guarantees. That length comes from `prctl` on Linux and from `rdvl` on Windows, which
reports whether SVE is present but not how wide it is. Compiling them
needs a compiler that accepts a fixed vector length: `clang-cl` does, in
its cc1 spelling, and `cl` does not, so an arm64 Windows build carries
SVE kernels only when clang-cl builds it.

The MSA kernel carries an asterisk the others do not. It is validated
against the official test vectors under qemu, byte-identical to the
x86-64 result, and its dispatch is validated both ways: an emulated core
without MSA falls back to scalar, one with it selects the kernel. No MSA
silicon has ever run it, and none is reachable, so it carries no
throughput number and should be treated as untested on hardware. CI
builds and tests it; no release archive contains it. It is
built by GCC through the vector-extension provider, because no std
provider deduces a vector width on this target and xsimd has no MSA
backend, and clang emits no MSA from the same source at all. Its static build
links a static glibc where the released archives link a static musl:
equally standalone, with no interpreter, no dynamic section and no
`GLIBC_` version symbol, so it carries no glibc floor either. The one
thing a static glibc gives up is NSS, which this tool never asks for.

## The wasm build

The wasm32-simd128 build is the one target where the fat-binary premise
cannot hold, because of how WebAssembly validates modules. On native
ISAs an unsupported instruction is harmless as long as it never
executes; that is exactly what lets one binary carry AVX-512 next to
SSE4.2 and choose at runtime. A wasm engine instead validates the
entire module at load time: if the engine does not support SIMD128, a
module containing any SIMD opcode is rejected before a single
instruction runs, whether those opcodes would ever have executed or
not.

So there is nothing to select between inside one module. Code that is
running has by definition already passed validation, so SIMD128 is
simply available (`available_arches()` reports it unconditionally
there). The scalar kernel is still compiled in and can be pinned for
comparison runs, but it is not a fallback: an engine without SIMD128
never gets far enough to reach it. Graceful degradation on wasm means
shipping two modules and picking one at load time on the embedder side
(feeding `WebAssembly.validate()` a small SIMD probe module is the
standard test). Every current mainstream engine supports SIMD128
(node 16+ has it on by default), which is why the release ships the
simd128 build only; `BLAKE3PP_WASM_SIMD128=OFF` (the
`wasm32-emcc-cxx23-scalar` preset) builds the other module.

The wasm build produces two kinds of executable. The tools, tests and
benches are node programs (`blake3pp::node_program`: host files through
NODERAWFS, a prespawned worker pool, exit when `main` returns). The
browser module, `web/blake3pp-web-<kernels>.mjs` plus its `.wasm`, is
the library's types behind embind under their own names, one ES module
usable from a page, a worker or node. Bytes are passed as (address,
length) into the module's memory, digests come back as hex, and a
scheduler is a `thread_pool` object the caller owns and passes, as C++
code passes a scheduler:

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

A pooled call blocks the thread the module was instantiated on until
the pool is done, so a page instantiates the module in a Web Worker
(a browser forbids waiting on its main thread) and needs
cross-origin isolation (`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`) for the SharedArrayBuffer
wasm threads run on. `b3pp.version()`, `b3pp.simd_provider()`,
`b3pp.execution_provider()`, `b3pp.compiled_arches()`,
`b3pp.available_arches()` and `b3pp.best_available()` report what `blake3ppsum --version` prints.
Plain mode only; keyed and derive_key modes are not bound.
