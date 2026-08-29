# tools/

Scripts around the build: the toolchain matrix, the release packaging,
and the binary inspection the fat-binary design keeps needing.
Everything runs from the repository root.

| Script | Purpose |
| --- | --- |
| `tc` | Run a CMake preset inside its per-compiler toolchain image (`tools/tc <preset>`, `tools/tc --shell <preset>`, `tools/tc --list`). |
| `gen-toolchains.py` | Regenerate the CMake toolchain files under `cmake/toolchains/` for the whole matrix. |
| `make-release.sh` | Assemble the static Linux release archives. |
| `build-msan-libcxx.sh` | Build the MemorySanitizer-instrumented libc++ the msan preset links. |
| `gen-test-vectors.py` | Turn the official BLAKE3 `test_vectors.json` into the C++ header the tests include. |
| `objscan.py` | Look inside a built object or binary: instruction classes, code-generation quality, and the kernel audit. |
| `kernel-audit.json` | The rules `objscan.py audit` enforces. |

## objscan.py

A fat binary raises the same questions on every compiler and
architecture: did the vector kernel vectorize, did the facade inline or
leave a call soup, does anything outside the kernels use an instruction
the dispatch verdict does not gate. `objscan.py` answers them from the
disassembly.

It reads ELF, Mach-O and PE/COFF (objects and linked binaries) and picks
the disassembler for the target: the prefixed GNU binutils in the cross
images, llvm-objdump elsewhere (Xcode's `objdump`, the LLVM install on
the Windows runners; `--objdump` names one explicitly). Function names
are the disassembler's demangled ones; patterns match on
`kern::<variant>::` rather than on whole names, since Mach-O keeps a
leading underscore and the MSVC demangler spells the anonymous namespace
its own way.

```sh
# Did it vectorize? The vector mnemonics in one kernel object.
tools/objscan.py mnemonics build/linux-gcc16-cxx26/CMakeFiles/blake3pp_kernel_avx2.dir/src/kernel/kernel.cpp.o
tools/objscan.py mnemonics --per-function BIN -x 'compress_in_place'

# How good is the code? Per hot function: size, vector density, loops
# (backward branches), calls that survived inlining with their callees,
# and vector traffic through the stack frame.
tools/objscan.py quality build/windows-msvc2026-cxx23/CMakeFiles/blake3pp_kernel_sse42.dir/src/kernel/kernel.cpp.obj

# Which functions use an instruction outside the kernel that owns it?
tools/objscan.py find build/linux-gcc16-cxx26/cli/blake3ppsum '^v[a-z]' -x 'kern::(avx2|avx512)' --fail

# The disassembly of one function.
tools/objscan.py disasm BIN 'kern::sve256::.*hash_batch'
```

`quality` is the check that catches the two classic failures: a compiler
that gave up inlining the SIMD facade (calls into `xsimd::` or
`std::simd` helpers in `hash_many`; MSVC's default inlining budget did
exactly that on the sse42 kernel until `/Ob3`), and rounds that were not
unrolled (a rolled kernel has a tenth of the vector instructions and one
more loop). The stack-vector column counts vector loads and stores
through the stack pointer; the transposed message blocks legitimately
live there, so read it as a trend across compilers, not as an assertion.

### The audit

```sh
tools/objscan.py audit build/<preset> [--binary build/<preset>/cli/blake3ppsum]
```

`kernel-audit.json` states, per architecture and kernel variant, the
instruction class the variant must contain, the classes it must not
(the dispatch verdict does not gate them: no AVX-512 in the avx2 kernel,
no SVE in the neon kernel), the quality thresholds for
the hot functions (no call outside the allow-list, a loop budget, a
minimum vector count in the widest function), and the classes that must
not appear in the linked binary outside the kernels that own them. A
rule matches an instruction by mnemonic or by operand text; `all: true`
asks for both.

Adding a kernel variant: add a `variants` entry under its architecture,
with a `match` regex against the variant name (`sve\d+`, `sve2_\d+`),
and extend the neighbouring variants' `forbid` lists if the new
instruction class must stay out of them. The audit reports a variant
without a rule and does not fail on it.
