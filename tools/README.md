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
| `amalgamate.py` | Concatenate the library into one translation unit, for Compiler Explorer and single-file drops. Its comments are stripped; the banner it writes is the one that stays. |
| `godbolt-link.py` | Shorten a source file into a Compiler Explorer link. |
| `build-try.py` | Write the "try it on Compiler Explorer" page for each example, minting a link when its source changed. |
| `godbolt-check.py` | Compile and run a source file on Compiler Explorer and report, for checking a snapshot before it is published. |
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

`build-site.sh` builds what `pysco68.github.io/blake3pp` serves. CI runs
the same script.

    tools/build-site.sh --no-link --serve      # localhost:8000

| Option | Effect |
| --- | --- |
| `--all` | Build every `v*` tag as well as `main`, and write the `versions.json` behind the version dropdown. |
| `--no-link` | Keep the Compiler Explorer link the repository already holds instead of generating a new one. An offline preview needs this. |
| `--serve` | Serve the result on port 8000. |

Every version is rendered with the tooling from the current checkout, not
with the tooling from the tag. A tag whose documents predate the current
navigation is skipped and does not appear in the dropdown.

`build-docs.sh` builds a single version. It stages three inputs into
`build/site-src` and runs MkDocs over them:

- the markdown documents in `docs/`;
- `README.md`, as the landing page;
- one reference page per public header, rendered by `doxygen-to-md.py`
  from the XML that Doxygen extracts from the `///` comments.

Staging keeps generated files out of the source tree. The links between
`README.md` and `docs/` are rewritten during staging for the site's flat
layout, and the originals keep working when the same files are read on
GitHub.

Two requirements: Doxygen as a system package (`apt install doxygen`),
and MkDocs, which `build-docs.sh` installs into a `.venv-docs`
virtualenv on first run.

## The "try it" pages

Compiler Explorer's compile nodes have no network, so a link there embeds
the source it was made from and cannot follow this repository. Each
example therefore gets a page under the published `/try/` that redirects to a link
built from that example, and the READMEs point at the page rather than at
the link.

`build-try.py` writes those pages. For each example it amalgamates the
library, appends the example, and hashes the result. The library goes in
with its comments stripped, because they describe a build this file is
not: the banner at the top says so once, and tells the reader to scroll
past the library to the program at the end, which keeps its own comments. A link is minted
only when that hash differs from the one recorded in the link manifest,
so rebuilding costs no requests, and `--no-link` never contacts
godbolt.org at all. The hash ignores the version stamp the amalgamation
writes into itself, which otherwise changes on every commit.

That manifest is published with the site, at `/try/links.json`, and the
next build reads it back from there (`--manifest-url`). A CI runner
starts from a fresh checkout but the site it deployed last time is still
standing, so the cache survives without being committed and without CI
needing write access to the repository. A local copy under `build/` is
the fallback, and `BLAKE3PP_SITE_URL` points the lookup at a fork's own
site. Three things can go wrong and none of them is fatal: the manifest
may not exist yet, which is the first run; the site may be unreachable,
which falls back to the local copy; and what comes back is checked entry
by entry, since it arrives over the network.

The documentation site does not use those pages. It is rebuilt from
scratch for every published version, so it can carry the real link, and
each version carries its own: `build-try.py --namespace <version>
--emit-map` writes the example-to-URL mapping, and `build-docs.sh
--links` substitutes it into that version's pages while staging. A reader
of `v0.1.0`'s documentation therefore opens `v0.1.0`'s code. Cache
entries are keyed by version, and a tag's sources cannot change, so its
links are minted once and then read from the manifest forever.

A try link standing alone in its paragraph becomes a button on the site.
In the READMEs it stays an ordinary link, which is what GitHub renders.

`godbolt-check.py` compiles and runs a file on Compiler Explorer and
reports what happened, which is how a snapshot gets verified before its
link is published.
