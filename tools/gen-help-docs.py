#!/usr/bin/env python3
"""Write or check the --help transcripts in docs/tools.md.

    python3 tools/gen-help-docs.py --bin-dir build/<preset>/cli           # rewrite
    python3 tools/gen-help-docs.py --bin-dir build/<preset>/cli --check   # exit 1 if stale

The transcripts come from the blake3ppsum_help_docs and blake3ppgen_help_docs
targets (`cmake --build build/<preset> --target help_docs`). They print the
machine- and build-dependent parts of --help as placeholders, so the output
is the same on every machine. A transcript is the fenced block between

    <!-- generated: <target> --help -->
    ...
    <!-- /generated -->

and the text outside those markers is not touched.
"""

import argparse
import difflib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "tools.md"
BLOCK = re.compile(
    r"(<!-- generated: (?P<cmd>[^\n]*?) -->\n)```\n(?P<body>.*?)```\n(<!-- /generated -->)",
    re.S)


def run(bin_dir: Path, cmd: str) -> str:
    target, *args = cmd.split()
    exe = bin_dir / (target + (".exe" if os.name == "nt" else ""))
    if not exe.is_file():
        sys.exit(f"gen-help-docs: {exe} not found; build it with "
                 "`cmake --build <build dir> --target help_docs`")
    # argv[0] is the shipping tool's name, which is what --help prints.
    name = target.removesuffix("_help_docs")
    out = subprocess.run([name, *args], executable=str(exe),
                         capture_output=True, text=True, check=True).stdout
    # CLI11 pads some lines with trailing spaces.
    lines = [line.rstrip() for line in out.replace("\r\n", "\n").split("\n")]
    return "\n".join(lines).rstrip("\n") + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin-dir", type=Path, required=True,
                    help="directory holding the *_help_docs executables")
    ap.add_argument("--check", action="store_true",
                    help="report staleness and write nothing")
    a = ap.parse_args()

    text = DOC.read_text(encoding="utf-8")
    blocks = list(BLOCK.finditer(text))
    if not blocks:
        sys.exit(f"gen-help-docs: no generated blocks in {DOC}")

    new = BLOCK.sub(
        lambda m: f"{m.group(1)}```\n{run(a.bin_dir, m.group('cmd'))}```\n{m.group(4)}",
        text)
    if new == text:
        return 0
    if a.check:
        sys.stdout.writelines(difflib.unified_diff(
            text.splitlines(keepends=True), new.splitlines(keepends=True),
            "docs/tools.md", "docs/tools.md (generated)"))
        print("docs/tools.md is stale: run tools/gen-help-docs.py "
              "--bin-dir <build dir>/cli and commit the result",
              file=sys.stderr)
        return 1
    DOC.write_text(new, encoding="utf-8")
    print(f"rewrote {len(blocks)} block(s) in docs/tools.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
