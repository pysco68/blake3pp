#!/usr/bin/env python3
"""Write the redirect page that stands between the README and a snapshot.

Compiler Explorer links embed the source they were made from: their
compile nodes have no network, so no link can follow this repository.
This page is the indirection that fixes that. The README points here
forever; CI repoints this at a freshly generated link whenever the
library changes, and says out loud which commit the link was built from,
so a stale one is visible rather than silent.

    tools/make-try-page.py --url https://godbolt.org/z/xxxx \\
        --commit abc1234 --output site/try/index.html
"""
import argparse
import datetime
import html
import pathlib

PAGE = """<!doctype html>
<meta charset="utf-8">
<title>blake3pp on Compiler Explorer</title>
<meta http-equiv="refresh" content="0; url={url}">
<link rel="canonical" href="{url}">
<style>
  body {{ font: 16px/1.6 system-ui, sans-serif; margin: 4rem auto; max-width: 42rem;
          padding: 0 1.5rem; color: #1a1d21; }}
  a {{ color: #b45309; }}
  .fine {{ color: #6b7280; font-size: .85rem; margin-top: 2rem; }}
</style>
<h1>blake3pp, in your browser</h1>
<p>Taking you to <a href="{url}">Compiler Explorer</a>. If nothing happens,
   follow that link.</p>
<p>It runs one amalgamated file: the scalar kernel, one vector kernel chosen
   by the compiler flags, and the multi-core path over
   <code>beman.execution</code>. The shipped library builds a translation unit
   per variant and dispatches between all of them at run time, which one file
   cannot do.</p>
<p class="fine">Built from <code>{commit}</code> on {date}. Compiler Explorer
   stores the source with the link, so this page is repointed whenever the
   library changes; the snapshot above is from that commit, not from the
   current tip. The source it runs is
   <a href="https://github.com/pysco68/blake3pp/blob/main/examples/blake3pp-single-file.cpp">examples/blake3pp-single-file.cpp</a>.</p>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    a = ap.parse_args()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(PAGE.format(
        url=html.escape(a.url, quote=True),
        commit=html.escape(a.commit),
        date=datetime.date.today().isoformat()))
    print(f"{a.output}: -> {a.url}")


if __name__ == "__main__":
    main()
