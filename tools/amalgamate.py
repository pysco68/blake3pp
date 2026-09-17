#!/usr/bin/env python3
"""One file of blake3pp, for pasting into Compiler Explorer.

A fat binary needs a translation unit per kernel variant, which one file
cannot have: the -m flags belong to the TU. So this emits the scalar
fallback the dispatch table requires plus ONE vector kernel, built for
whatever the reader selected in the compiler flags. It is a demonstration
of the API, not of runtime dispatch, and the file says so.

The rules, all four of them:

  * kernel-internal headers are PASTED, not included: they are `#pragma
    once` and each variant needs its own copy;
  * they are pasted at the include site, recursively, so the facade's own
    #if still chooses the SIMD provider;
  * BLAKE3PP_ARCH_NS is substituted textually, which is the flattening;
  * two variant-neutral headers stay ordinary includes, so their guards
    keep doing their job.

xsimd is not needed (std::simd is the provider) and neither is stdexec:
the scheduler comes from beman.execution, which Compiler Explorer already
installs, and only the caller's own TU includes it.

    tools/amalgamate.py -o /tmp/blake3pp-single-file.cpp
"""
import argparse
import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent
INCLUDE = re.compile(r'^#include "((?:kernel|core|io|dispatch)/[^"]+)"[^\n]*$', re.M)
SHARED = {"kernel/kernel.hpp", "kernel/force_inline.hpp"}
# Everything the library needs beyond the kernels, in link order.
# The kernel is registered under a real variant name because dispatch
# gates on that name: registering AVX-512 code as "avx2" would enable it
# on a machine that cannot run it. So the emitted file insists that the
# compiler was actually told to target the variant it carries.
# The variant is whatever the compiler was told to target, so the file
# asks the compiler rather than the generator. Widest first.
SELECT = """
#if defined(__AVX512F__)
#define BLAKE3PP_AMALGAM_NS avx512
#elif defined(__AVX2__)
#define BLAKE3PP_AMALGAM_NS avx2
#elif defined(__SSE4_2__)
#define BLAKE3PP_AMALGAM_NS sse42
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define BLAKE3PP_AMALGAM_NS neon
#elif defined(__wasm_simd128__)
#define BLAKE3PP_AMALGAM_NS simd128
#endif
"""
SUPPORT = ["src/blake3pp.cpp", "src/dispatch/dispatch.cpp",
           "src/dispatch/transpose16.cpp", "src/dispatch/cpu_detect_x86.cpp",
           "src/io/file_reader.cpp", "src/io_hash.cpp"]
PUBLIC = ["include/blake3pp/dispatch.hpp", "include/blake3pp/core.hpp",
          "include/blake3pp/detail/file_reader.hpp",
          "include/blake3pp/detail/file_writer.hpp",
          "include/blake3pp/io.hpp"]


GLOBAL = set()          # non-kernel internals: emitted once for the file


def inline(rel, seen, variant):
    text = (REPO / "src" / rel).read_text().replace("#pragma once", "")
    text = re.sub(r'#ifndef BLAKE3PP_ARCH_NS\n#error[^\n]*\n#endif\n', "", text)

    def paste(m):
        target = m.group(1)
        if target in SHARED:
            return ""          # emitted once, above
        once = seen if target.startswith("kernel/") else GLOBAL
        if target in once:
            return ""
        once.add(target)
        return inline(target, seen, variant)

    return INCLUDE.sub(paste, text).replace("BLAKE3PP_ARCH_NS", variant)


def strip_comments(text: str) -> str:
    """Drop C and C++ comments, leaving string literals alone.

    The pasted library is nine thousand lines of a reader's screen, and
    its comments document a build this file is not: the shipped library
    has a translation unit per kernel, and this has one. The banner at
    the top says so, and says it once.

    A block comment becomes as many newlines as it spanned, so a macro
    continuation cannot swallow the line after it.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j            # the newline itself is kept
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            # A raw string ends at its own delimiter, and anything at all
            # may appear inside it.
            if c == '"' and text[max(0, i - 1):i] == "R":
                k = text.find("(", i)
                delim = text[i + 1:k]
                close = ")" + delim + '"'
                j = text.find(close, k)
                j = n if j < 0 else j + len(close)
            else:
                j = i + 1
                while j < n:
                    if text[j] == "\\":
                        j += 2
                        continue
                    if text[j] == c:
                        j += 1
                        break
                    j += 1
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    # Comments leave holes; close them up rather than shipping the gaps.
    lines = [line.rstrip() for line in "".join(out).split("\n")]
    tidy, blanks = [], 0
    for line in lines:
        blanks = blanks + 1 if not line else 0
        if blanks < 2:
            tidy.append(line)
    return "\n".join(tidy)


def version():
    try:
        return subprocess.run(["git", "-C", str(REPO), "describe", "--always", "--dirty"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--no-demo", action="store_true", help="omit main()")
    ap.add_argument("--demo-name", default="",
                    help="what the program at the end of the file is called")
    ap.add_argument("--keep-comments", action="store_true",
                    help="leave the library's own comments in place")
    args = ap.parse_args()

    # Whatever ends up at the bottom, the reader has to be told it is
    # there: everything above it is nine thousand lines of library.
    demo = args.demo_name or ("" if args.no_demo else "the demo")
    notice = ""
    if demo:
        notice = f"""
//
// {'=' * 70}
//
//     >>>>>>>>>>   S C R O L L   T O   T H E   B O T T O M   <<<<<<<<<<
//
//     The code of {demo} is in main(), at the END of this file.
//     Everything between here and there is the library, inlined.
//
// {'=' * 70}"""

    arch_def = (REPO / "include/blake3pp/detail/arch.def").read_text()
    out = [f"""// blake3pp, amalgamated from {version()} by tools/amalgamate.py.{notice}
//
// Two kernels: the scalar fallback, and {"BLAKE3PP_AMALGAM_NS"}, built for whatever
// this compiler was told to target. The shipped library compiles one
// translation unit per variant and dispatches between all of them at run
// time; one file cannot, so this carries one. Everything else is the real
// library, unmodified.
//
// Compiler Explorer: x86-64 gcc 16.1, 16.2 or trunk (older GCCs have
// no <simd>; the file detects that and carries the scalar kernel only)
//   -std=c++26 -O2 -msse4.2        on x86
//   -std=c++26 -O2                 on aarch64, where NEON is mandatory
// and, for the multi-core example only, the beman.execution library.
//
// Which vector kernel this carries is decided by those flags, not by the
// generator. Choose them for the machine that will RUN it: one file means
// one baseline, so everything here including dispatch is emitted for the
// target you name, and a binary built for a wider one dies with an
// illegal instruction on a narrower CPU before dispatch can choose.
//
//   -msse4.2   on every x86-64 part since roughly 2009, and part of the
//              x86-64-v2 baseline: safe essentially everywhere
//   (none)     on aarch64 NEON is in the architecture, so it is selected
//              and is always runnable
//   -mavx2     Haswell and Zen onward: common, not universal
//   -mavx512f  narrow availability, absent from most desktop parts
//
// The shipped library carries no such constraint. It compiles one
// translation unit per variant, leaves the program baseline alone, and
// chooses between them at run time.
#include <cstddef>
#include <cstdint>
#include <span>

#include <version>
{SELECT}
#if defined(__has_include) && __has_include(<simd>)
#define BLAKE3PP_HAS_STD_SIMD 1
#else
#define BLAKE3PP_AMALGAM_SCALAR_ONLY 1   // no <simd> here
#endif
"""]
    # The public headers include each other and the arch table; paste all
    # three in dependency order and drop the includes between them.
    for h in PUBLIC:
        text = (REPO / h).read_text().replace("#pragma once", "")
        text = text.replace("#include <blake3pp/detail/arch.def>", arch_def)
        text = re.sub(r'#include [<"]blake3pp/[^>"]+[>"][^\n]*\n', "", text)
        out.append(text.rstrip() + "\n")
    # Variant-neutral, so once, before either kernel copy.
    for h in ("kernel/force_inline.hpp", "kernel/kernel.hpp"):
        text = (REPO / "src" / h).read_text().replace("#pragma once", "")
        text = re.sub(r'#include [<"](kernel|blake3pp)/[^>"]+[>"][^\n]*\n', "", text)
        out.append(text.rstrip() + "\n")

    # The build generates these two; inline them instead.
    registry = ("BLAKE3PP_KERNEL(scalar)\n"
                "#ifdef BLAKE3PP_AMALGAM_NS\n"
                "BLAKE3PP_KERNEL(BLAKE3PP_AMALGAM_NS)\n#endif\n")
    stamp = f'#define BLAKE3PP_STAMPED_VERSION "{version()}-amalgamated"\n'
    out += ["#define BLAKE3PP_FORCE_SCALAR 1",
            inline("kernel/kernel.cpp", set(), "scalar")]
    out += ["#if defined(BLAKE3PP_AMALGAM_NS) && !defined(BLAKE3PP_AMALGAM_SCALAR_ONLY)",
            "#undef BLAKE3PP_FORCE_SCALAR",
            inline("kernel/kernel.cpp", set(), "BLAKE3PP_AMALGAM_NS"),
            "#endif"]
    for f in SUPPORT:
        text = inline(f[len("src/"):], set(), "BLAKE3PP_AMALGAM_NS")
        text = text.replace('#include "blake3pp_version_stamp.hpp"', stamp)
        text = text.replace('#include "blake3pp_kernel_registry.inc"', registry)
        # arch.def is an X-macro table, not a header: it is included at
        # several places on purpose and each one must keep its copy.
        text = text.replace("#include <blake3pp/detail/arch.def>", arch_def)
        text = re.sub(r'#include [<"]blake3pp/[^>"]+[>"][^\n]*\n', "", text)
        text = re.sub(r'#include "kernel/kernel.hpp"[^\n]*\n', "", text)
        out.append(text)
    # The multi-core half, when the reader selected beman.execution in
    # Compiler Explorer's library list. Header-only, so it only has to be
    # reachable; nothing else in this file depends on it.
    par = (REPO / "include/blake3pp/parallel.hpp").read_text().replace("#pragma once", "")
    par = re.sub(r'#include [<"]blake3pp/[^>"]+[>"][^\n]*\n', "", par)
    out.append("""
#if defined(__has_include) && __has_include(<beman/execution/execution.hpp>)
#define BLAKE3PP_EXECUTION_BEMAN 1
#define BLAKE3PP_HAS_STD_THREAD 1
#ifndef BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND
#define BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND 1
#endif
#define BLAKE3PP_AMALGAM_HAS_PARALLEL 1
""" + par + """
#endif  // beman.execution
""")

    # The banner is the one comment block that survives, and a program
    # appended at the end keeps its own: it is the part being read.
    banner, body = out[0], "\n".join(out[1:])
    if not args.keep_comments:
        body = strip_comments(body)
    text = banner + "\n" + body
    if not args.no_demo:
        text += "\n" + (REPO / "tools/amalgam-demo.inc").read_text()
    pathlib.Path(args.output).write_text(text)
    print(f"{args.output}: {len(pathlib.Path(args.output).read_text().splitlines())} lines")


if __name__ == "__main__":
    main()
