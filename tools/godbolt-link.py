#!/usr/bin/env python3
"""Turn an amalgamated file into a Compiler Explorer short link.

Posts the editor state to godbolt.org's shortener and prints the URL, so
the README can link a live, runnable example rather than describing one.

    tools/amalgamate.py --no-demo -o /tmp/lib.cpp
    cat /tmp/lib.cpp examples/parallel-demo.cpp > /tmp/full.cpp
    tools/godbolt-link.py /tmp/full.cpp --lib beman_execution:trunk
"""
import argparse
import json
import pathlib
import sys
import urllib.request

API = "https://godbolt.org/api/shortener"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=pathlib.Path)
    ap.add_argument("--compiler", default="g162", help="Compiler Explorer compiler id")
    ap.add_argument("--options", default="-std=c++26 -O2 -msse4.2")
    ap.add_argument("--lib", action="append", default=[],
                    help="name:version, repeatable")
    args = ap.parse_args()

    libs = []
    for spec in args.lib:
        name, _, ver = spec.partition(":")
        libs.append({"id": name, "version": ver or "trunk"})
    compiler = {"id": args.compiler, "options": args.options, "libs": libs}
    state = {"sessions": [{"id": 1, "language": "c++",
                           "source": args.source.read_text(),
                           "compilers": [compiler],
                           "executors": [{"compiler": compiler}]}]}
    body = json.dumps(state).encode()
    req = urllib.request.Request(API, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        print(json.load(r)["url"])


if __name__ == "__main__":
    sys.exit(main())
