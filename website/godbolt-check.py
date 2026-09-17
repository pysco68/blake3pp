#!/usr/bin/env python3
"""Compile and run a source file on Compiler Explorer, and report.

A "try it" link is only worth publishing if the snapshot behind it still
builds and runs on the machines that will serve it. This asks Compiler
Explorer to do both and exits non-zero when either fails.

    website/godbolt-check.py /tmp/full.cpp --lib beman_execution:trunk
"""
import argparse
import json
import pathlib
import sys
import urllib.error
import urllib.request

API = "https://godbolt.org/api/compiler/{compiler}/compile"


def lines(entries):
    return [e.get("text", "") for e in (entries or [])]


def check(source: pathlib.Path, compiler: str, options: str, libs: list,
          timeout: int):
    """Returns (ok, build_log, run_log)."""
    body = {
        "source": source.read_text(),
        "lang": "c++",
        "allowStoreCodeDebug": False,
        "options": {
            "userArguments": options,
            "executeParameters": {"args": [], "stdin": ""},
            "filters": {"execute": True, "commentOnly": True, "trim": True},
            "libraries": [{"id": n, "version": v or "trunk"}
                          for n, _, v in (spec.partition(":") for spec in libs)],
            "tools": [],
        },
    }
    req = urllib.request.Request(
        API.format(compiler=compiler),
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        result = json.load(r)

    build = lines(result.get("stderr"))
    run = result.get("execResult") or {}
    run_log = lines(run.get("stdout")) + lines(run.get("stderr"))
    ok = result.get("code") == 0 and run.get("code") == 0
    return ok, build, run_log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=pathlib.Path)
    ap.add_argument("--compiler", default="g162",
                    help="Compiler Explorer compiler id")
    ap.add_argument("--options", default="-std=c++26 -O2 -msse4.2")
    ap.add_argument("--lib", action="append", default=[],
                    help="name:version, repeatable")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--quiet", action="store_true",
                    help="print the program's output only on failure")
    a = ap.parse_args()

    try:
        ok, build, run = check(a.source, a.compiler, a.options, a.lib, a.timeout)
    except (urllib.error.URLError, TimeoutError) as e:
        print(f"godbolt-check: {a.source.name}: unreachable ({e})", file=sys.stderr)
        return 2

    if not ok or not a.quiet:
        for line in build[:40]:
            print(f"  {line}")
        for line in run[:40]:
            print(f"  {line}")
    print(f"{a.source.name}: {'ok' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
