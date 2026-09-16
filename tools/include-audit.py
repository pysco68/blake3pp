#!/usr/bin/env python3
"""Which headers a translation unit uses, checked across configurations.

clang-include-cleaner sees exactly one build. That is enough to trust what
it says is MISSING, since a name used here must be declared somewhere, but
not what it says is UNUSED: this library's sources are full of code that
only exists on another architecture, another kernel or another standard
library, and a header that looks idle in one build is load-bearing in the
next. Removing on one build's word is how `dispatch/cpu_detect.hpp`
disappears from five per-architecture files that still need it.

So: additions are the union over the builds given, removals the
intersection, and a short keep-list covers what no Linux build can ever
prove.

    tools/include-audit.py                       # the include-audit preset
    tools/include-audit.py -b build/a -b build/b # and any others
    tools/include-audit.py --apply               # insert what is missing

--apply only ever inserts. Removals are printed for a human, because the
intersection narrows the guess without making it safe: no build tree here
compiles the Windows or macOS backends.
"""
import argparse
import collections
import json
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
TOOL = os.environ.get("CLANG_INCLUDE_CLEANER", "clang-include-cleaner-22")

# Headers to keep whatever the analysis says, with the reason. Every entry
# is a case where the need exists in a build that cannot be run here.
KEEP = {
    "<ostream>":
        "MSVC's <string_view> declares the stream insertion but leaves "
        "basic_ostream incomplete, so clang-cl needs it for a CAPTURE of a "
        "string_view; libstdc++ and libc++ pull it in transitively",
}

STD = re.compile(r"^#include <[^/]+>$")


def translation_units(build):
    db = json.loads((pathlib.Path(build) / "compile_commands.json").read_text())
    out = []
    for entry in db:
        f = entry["file"]
        if "/_deps/" in f or "/thirdparty/" in f or f in out:
            continue
        out.append(f)
    return out


def analyse(build, files):
    adds, drops = collections.defaultdict(set), collections.defaultdict(set)
    for f in files:
        try:
            res = subprocess.run(
                [TOOL, "-p", str(build), "--print=changes", f],
                capture_output=True, text=True, timeout=600)
        except FileNotFoundError:
            sys.exit(f"include-audit: {TOOL} not found")
        rel = os.path.relpath(f, REPO)
        for line in res.stdout.splitlines():
            if line.startswith("+ "):
                adds[rel].add(line[2:].strip())
            elif line.startswith("- "):
                drops[rel].add(line[2:].split("@Line:")[0].strip())
    return adds, drops


def insert(rel, headers):
    """Put standard headers in the file's own standard-header block."""
    path = REPO / rel
    lines = path.read_text().split("\n")
    idx = [i for i, l in enumerate(lines) if STD.match(l)]
    if idx:
        start = end = idx[0]
        while end + 1 < len(lines) and STD.match(lines[end + 1]):
            end += 1
        have = {l[len("#include "):] for l in lines[start:end + 1]}
        lines[start:end + 1] = [f"#include {h}" for h in sorted(have | headers)]
    else:
        first = next(i for i, l in enumerate(lines) if l.startswith("#include"))
        last = first
        while last + 1 < len(lines) and lines[last + 1].startswith("#include"):
            last += 1
        lines[last + 1:last + 1] = [""] + [f"#include {h}" for h in sorted(headers)]
    path.write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-b", "--build", action="append", default=[],
                    help="a build directory holding compile_commands.json")
    ap.add_argument("--apply", action="store_true",
                    help="insert the missing standard headers")
    args = ap.parse_args()
    builds = args.build or [REPO / "build" / "include-audit"]

    union, common = collections.defaultdict(set), None
    for b in builds:
        b = pathlib.Path(b)
        if not (b / "compile_commands.json").exists():
            sys.exit(f"include-audit: no compile_commands.json in {b}")
        files = translation_units(b)
        print(f"-- {b.name}: {len(files)} translation units", file=sys.stderr)
        adds, drops = analyse(b, files)
        for k, v in adds.items():
            union[k] |= v
        common = dict(drops) if common is None else {
            k: common.get(k, set()) & v for k, v in drops.items()}

    # A standard header is <name> with no path in it. <asm/unistd_64.h> and
    # <exec/static_thread_pool.hpp> are angled too, and belong in the block
    # with their neighbours rather than among <span> and <cstdint>.
    std = lambda h: h.startswith("<") and "/" not in h
    std_adds = {k: {h for h in v if std(h)} for k, v in union.items()}
    # <stdio.h> when the file already has <cstdio>: the tool names the header
    # that declares the symbol, not the spelling C++ uses for it.
    for f, heads in std_adds.items():
        text = (REPO / f).read_text()
        heads -= {h for h in heads
                  if h.endswith(".h>") and f"#include <c{h[1:-3]}>" in text}
    std_adds = {k: v for k, v in std_adds.items() if v}
    own_adds = {k: {h for h in v if not std(h)} for k, v in union.items()}
    own_adds = {k: v for k, v in own_adds.items() if v}

    print(f"\nmissing standard headers ({sum(len(v) for v in std_adds.values())}):")
    for f in sorted(std_adds):
        print(f"  {f}: {' '.join(sorted(std_adds[f]))}")
        if args.apply:
            insert(f, std_adds[f])

    print(f"\nmissing project headers, for a human ({sum(len(v) for v in own_adds.values())}):")
    for f in sorted(own_adds):
        print(f"  {f}: {' '.join(sorted(own_adds[f]))}")

    kept = collections.defaultdict(set)
    print("\nunused in every build given, for a human:")
    any_drop = False
    for f in sorted(common or {}):
        left = set()
        for h in sorted(common[f]):
            if h in KEEP:
                kept[h].add(f)
            else:
                left.add(h)
        if left:
            any_drop = True
            print(f"  {f}: {' '.join(sorted(left))}")
    if not any_drop:
        print("  (none)")
    for h, files in kept.items():
        print(f"\nkept, {len(files)} file(s): {h}\n  {KEEP[h]}")

    if args.apply:
        print("\napplied the standard headers above; nothing was removed")


if __name__ == "__main__":
    main()
