# tools/

Scripts around the build: the toolchain matrix, the CI entry point, the
release packaging, the documentation site, and the binary inspection the
fat-binary design keeps needing. Everything runs from the repository
root.

| Script | Purpose |
| --- | --- |
| `tc` | Run a CMake preset inside its per-compiler toolchain image (`tools/tc <preset>`, `tools/tc --shell <preset>`, `tools/tc --list`). |
| `gen-toolchains.py` | Regenerate the CMake toolchain files under `cmake/toolchains/` for the whole matrix. |
| `gen-environments.py` | Regenerate the cmake-re environment files next to the toolchain files, keyed by the toolchain image tag. |
| `preset-args.py` | Print the configure arguments a CMake preset stands for, for tools that do not read presets (cmake-re). |
| `toolchain-image-tag.sh` | The content hash that names the toolchain images. |
| `ci-test.sh` | The CI entry point: configure, build, audit and run the preset's full test matrix (every emulator configuration its architecture supports). |
| `make-release.sh` | Assemble the static Linux release archives. |
| `build-msan-libcxx.sh` | Build the MemorySanitizer-instrumented libc++ the msan preset links. |
| `gen-test-vectors.py` | Turn the official BLAKE3 `test_vectors.json` into the C++ header the tests include. |
| `objscan.py` | Look inside a built object or binary: instruction classes, code-generation quality, and the kernel audit. |
| `kernel-audit.json` | The rules `objscan.py audit` enforces. |
| `amalgamate.py` | Concatenate the library into one translation unit, for Compiler Explorer and single-file drops. |
| `godbolt-link.py` | Shorten a source file into a Compiler Explorer link. |
| `make-try-page.py` | Write the redirect page the README's "try it" link points at. |
| `include-audit.py` | Cross-build `clang-include-cleaner` pass: additions from the union, removals only from the intersection. |
| `build-site.sh` | Build the published documentation site: every release plus `main`, and the Compiler Explorer link (`--all`, `--no-link`, `--serve`). |
| `build-docs.sh` | Build one version of that site into a directory. |
| `doxygen-to-md.py` | Turn Doxygen's XML into the site's reference pages. |
| `Doxyfile` | The Doxygen configuration, XML only: no Doxygen HTML is published. |

## objscan.py

A fat binary raises the same questions on every compiler and
architecture: did the vector kernel vectorize, did the facade inline or
leave a call soup, does anything outside the kernels use an instruction
the dispatch verdict does not gate, whose address is that in the
emulator's trace. `objscan.py` answers them from the disassembly.

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
tools/objscan.py find build/linux-s390x-gcc15-cxx23/cli/blake3ppsum '^(vlbr|vnx|vnn|voc|vmsl)' -x 'kern::vxe' --fail

# The disassembly of one function.
tools/objscan.py disasm BIN 'kern::sve256::.*hash_batch'

# Which instruction crashes under this emulator configuration? The
# mnemonics one binary has and another lacks, with their users.
tools/objscan.py diff bench-arch13 bench-z13

# Whose address is that? (qemu -d in_asm prints load-biased addresses.)
tools/objscan.py resolve build/linux-riscv64-gcc15-cxx23/cli/blake3ppsum 0x5555556a0b2c --bias 0x555555554000
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
no SVE in the neon kernel, no Zvbb in the plain rvv kernels, no z14/z15
vector instruction outside the vxe kernel), the quality thresholds for
the hot functions (no call outside the allow-list, a loop budget, a
minimum vector count in the widest function), and the classes that must
not appear in the linked binary outside the kernels that own them. A
rule matches an instruction by mnemonic or by operand text; `all: true`
asks for both.

`ci-test.sh` runs the audit on every Linux preset after the build (the
wasm presets excepted: a wasm module has no objdump), the Windows and
macOS jobs run it on theirs. Windows executables carry no symbol table,
so the binary-level check runs on the object files only there.

Adding a kernel variant: add a `variants` entry under its architecture,
with a `match` regex against the variant name (`sve\d+`, `rvv\d+_zvbb`),
and extend the neighbouring variants' `forbid` lists if the new
instruction class must stay out of them. The audit reports a variant
without a rule and does not fail on it.

## The documentation site

`build-site.sh` produces what `pysco68.github.io/blake3pp` serves, and CI
runs the same script, so a local preview cannot drift from the published
site:

    tools/build-site.sh --no-link --serve      # localhost:8000

`--no-link` keeps the Compiler Explorer link the repository already holds
instead of minting a new one, which is what an offline preview wants;
drop it to regenerate the link. `--all` additionally builds every `v*`
tag into its own directory and writes the `versions.json` behind the
version dropdown. Each version is rendered with the tooling from the
current checkout, not from the tag, so there is only ever one renderer to
maintain; a tag whose documents predate the current navigation is skipped
rather than published half-built.

One version comes from `build-docs.sh`, which stages three inputs into
`build/site-src` and runs MkDocs over them: the markdown in `docs/`, the
README as the landing page, and the public headers' `///` comments by way
of Doxygen's XML and `doxygen-to-md.py`. Nothing is generated into the
source tree, and the cross-links between README and `docs/` are rewritten
for the site's flat layout so the same links keep working on GitHub.

MkDocs lives in a `.venv-docs` virtualenv the script creates on first
run; Doxygen is a system package (`apt install doxygen`).
