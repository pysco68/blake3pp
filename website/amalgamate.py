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

    website/amalgamate.py -o /tmp/blake3pp-single-file.cpp
"""
import argparse
import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent
INCLUDE = re.compile(r'^#include "((?:kernel|core|io|dispatch)/[^"]+)"[^\n]*$', re.M)
# Variant-neutral kernel headers: pasted once above both kernel copies,
# because what they define lives outside the per-variant namespace and a
# second copy is a redefinition. Every other kernel header is inside
# blake3pp::kern::BLAKE3PP_ARCH_NS and is pasted once per variant.
SHARED = {"kernel/kernel.hpp", "kernel/force_inline.hpp",
          "kernel/endian.hpp"}
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
# Everything the library needs beyond the kernels, in link order.
# src/diagnosis.cpp reads driver_stats and nothing else, so unlike
# probe_file_io() below it needs no execution provider. Its declarations
# are in the public trace.hpp, and without it each would be a link error.
SUPPORT = ["src/blake3pp.cpp", "src/dispatch/dispatch.cpp",
           "src/dispatch/cpu_detect_x86.cpp", "src/diagnosis.cpp",
           "src/io/file_reader.cpp", "src/io/io_engines.cpp",
           "src/io/stream_reader.cpp", "src/io_hash.cpp"]
PUBLIC = ["include/blake3pp/detail/contract.hpp",
          "include/blake3pp/dispatch.hpp", "include/blake3pp/core.hpp",
          "include/blake3pp/trace.hpp",
          "include/blake3pp/detail/driver.hpp",
          "include/blake3pp/detail/io_geometry.hpp",
          "include/blake3pp/detail/file_reader.hpp",
          "include/blake3pp/detail/file_writer.hpp",
          # The stream driver and the completion half it borrows. They are
          # pasted here, not left to the parallel block's stripped
          # includes: update_stream() is the one entry point there whose
          # definition is compiled, and a reader who enabled the provider
          # would otherwise reach it as a link error.
          "include/blake3pp/detail/memory_driver.hpp",
          "include/blake3pp/detail/pipe_driver.hpp",
          "include/blake3pp/io.hpp"]


GLOBAL = set()          # non-kernel internals: emitted once for the file

# Definitions a single file cannot carry, and what to say where they
# were. The file pipeline (detail/file_pipeline.hpp) is written against
# a sender/receiver provider and the single file has none of its own:
# its API is the sequential half, which is why file_pipeline.hpp is not
# in PUBLIC above and parallel_io.hpp is emitted only behind
# BLAKE3PP_AMALGAM_HAS_PARALLEL. probe_file_io() is the one definition
# in the provider-free sources that reaches across that line (it wants
# the plan update_file() would have made), so it is cut here instead of
# failing at the paste site.
OMIT = {"src/io_hash.cpp": [("file_io_probe probe_file_io(",
                             "probe_file_io(): it needs the file pipeline's "
                             "plan, which needs an execution provider")]}


def drop_definition(text, head, why):
    """Cut every definition whose first line starts with `head`.

    Brace-counted from the signature rather than regex-matched over the
    body, and the comment block immediately above goes with it: a
    definition's explanation describes code this file does not have.
    """
    while True:
        at = text.find("\n" + head)
        if at < 0:
            return text
        start = at + 1
        # The comment block attached above, if any.
        while True:
            prev = text.rfind("\n", 0, start - 1) + 1
            if prev == start or not text[prev:start].lstrip().startswith("//"):
                break
            start = prev
        depth, i, opened = 0, text.index("{", at), False
        while i < len(text):
            if text[i] == "{":
                depth, opened = depth + 1, True
            elif text[i] == "}":
                depth -= 1
                if opened and depth == 0:
                    break
            i += 1
        end = text.index("\n", i) + 1
        text = text[:start] + f"// Omitted from the single file: {why}.\n" + text[end:]



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

    The pasted library is many thousand lines of a reader's screen, and
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
    # there: everything above it is many thousand lines of library.
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
    io_engine_def = (REPO / "include/blake3pp/detail/io_engine.def").read_text()
    out = [f"""// blake3pp, amalgamated from {version()} by website/amalgamate.py.{notice}
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
// plus the beman.execution library, which Compiler Explorer installs and
// which is the easy way to get an execution provider there. Select it and
// the multi-core entry points exist; leave it out and they do not, and
// execution_provider() answers "none".
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
// GCC 16 ships <simd> at every standard but defines std::simd only from
// C++26, and there is no feature-test macro to ask, so the language
// version is the gate. Below it the file carries the scalar kernel alone:
// the library proper has a CMake probe and other providers, a single file
// has neither.
#if defined(__has_include) && __has_include(<simd>) && __cplusplus > 202302L
#define BLAKE3PP_HAS_STD_SIMD 1
#else
#define BLAKE3PP_AMALGAM_SCALAR_ONLY 1   // no usable <simd> here
#endif

// The multi-core half, when the reader selected beman.execution in
// Compiler Explorer's library list. Decided here rather than beside the
// headers it enables, because the introspection in dispatch.cpp is
// compiled long before those and has to see the same answer.
#if defined(__has_include) && __has_include(<beman/execution/execution.hpp>)
#define BLAKE3PP_EXECUTION_BEMAN 1
#define BLAKE3PP_HAS_STD_THREAD 1
#ifndef BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND
#define BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND 1
#endif
#define BLAKE3PP_AMALGAM_HAS_PARALLEL 1
#endif
"""]
    # The public headers include each other and the arch table; paste all
    # three in dependency order and drop the includes between them.
    for h in PUBLIC:
        text = (REPO / h).read_text().replace("#pragma once", "")
        text = text.replace("#include <blake3pp/detail/arch.def>", arch_def)
        text = text.replace("#include <blake3pp/detail/io_engine.def>",
                            io_engine_def)
        text = re.sub(r'#include [<"]blake3pp/[^>"]+[>"][^\n]*\n', "", text)
        out.append(text.rstrip() + "\n")
    # Variant-neutral, so once, before either kernel copy.
    for h in ("kernel/force_inline.hpp", "kernel/endian.hpp",
              "kernel/kernel.hpp"):
        text = (REPO / "src" / h).read_text().replace("#pragma once", "")
        text = re.sub(r'#include [<"](kernel|blake3pp)/[^>"]+[>"][^\n]*\n', "", text)
        out.append(text.rstrip() + "\n")

    # The build generates these two; inline them instead.
    # The second kernel is registered under exactly the condition that
    # emits it: dispatch would otherwise name a variant whose ops table
    # this file does not carry, which is a link error.
    registry = ("BLAKE3PP_KERNEL(scalar)\n"
                "#if defined(BLAKE3PP_AMALGAM_NS) && "
                "!defined(BLAKE3PP_AMALGAM_SCALAR_ONLY)\n"
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
        for head, why in OMIT.get(f, ()):
            text = drop_definition(text, head, why)
        text = text.replace('#include "blake3pp_version_stamp.hpp"', stamp)
        text = text.replace('#include "blake3pp_kernel_registry.inc"', registry)
        # arch.def and io_engine.def are X-macro tables, not headers:
        # each is included at several places on purpose and every one
        # must keep its copy.
        text = text.replace("#include <blake3pp/detail/arch.def>", arch_def)
        text = text.replace("#include <blake3pp/detail/io_engine.def>",
                            io_engine_def)
        text = re.sub(r'#include [<"]blake3pp/[^>"]+[>"][^\n]*\n', "", text)
        text = re.sub(r'#include "kernel/kernel.hpp"[^\n]*\n', "", text)
        out.append(text)
    # The multi-core half, when the reader selected beman.execution in
    # Compiler Explorer's library list. Header-only, so it only has to be
    # reachable; nothing else in this file depends on it.
    # parallel.hpp, then the file overloads that need both it and io.hpp.
    par = ""
    for h in ("include/blake3pp/parallel.hpp", "include/blake3pp/parallel_io.hpp"):
        text = (REPO / h).read_text().replace("#pragma once", "")
        par += re.sub(r'#include [<"]blake3pp/[^>"]+[>"][^\n]*\n', "", text)
    out.append("""
#ifdef BLAKE3PP_AMALGAM_HAS_PARALLEL
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
        text += "\n" + (REPO / "website/amalgam-demo.inc").read_text()
    pathlib.Path(args.output).write_text(text)
    print(f"{args.output}: {len(pathlib.Path(args.output).read_text().splitlines())} lines")


if __name__ == "__main__":
    main()
