#!/usr/bin/env python3
"""Write the "try it on Compiler Explorer" pages, one per example.

A Compiler Explorer link embeds the source it was made from, because
their compile nodes have no network and cannot fetch this repository. The
pages written here are the indirection that fixes that: the READMEs point
at a page, and this repoints the page at a freshly generated link when the
source behind it changes.

Each page's source is the amalgamated library with one example appended,
so what opens on Compiler Explorer is the example as it is committed.

    tools/build-try.py --output site/try
    tools/build-try.py --output site/try --no-link   # offline: reuse links
"""
import argparse
import datetime
import hashlib
import html
import json
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# The example that opens from the site root, kept at /try/ because the
# README has pointed there since before the examples existed.
ROOT = {
    "slug": "",
    "title": "blake3pp, in your browser",
    "source": "examples/parallel-demo.cpp",
    "libs": ["beman_execution:trunk"],
    "blurb": "It hashes on one core and on all of them, and prints both rates.",
}

# The multi-core example is the only one needing a library: the scheduler
# comes from beman.execution, which Compiler Explorer installs.
NEEDS_EXECUTION = "06-multi-core"

PAGE = """<!doctype html>
<meta charset="utf-8">
<title>{title}</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="0; url={url}">
<link rel="canonical" href="{url}">
<style>
  body {{ font: 16px/1.6 system-ui, sans-serif; margin: 4rem auto; max-width: 42rem;
          padding: 0 1.5rem; color: #1a1d21; }}
  a {{ color: #b45309; }}
  .fine {{ color: #6b7280; font-size: .85rem; margin-top: 2rem; }}
</style>
<h1>{title}</h1>
<p>Taking you to <a href="{url}">Compiler Explorer</a>. If nothing happens,
   follow that link.</p>
<p>{blurb}</p>
<p>It runs one amalgamated file: the scalar kernel, one vector kernel chosen
   by the compiler flags, and the file and multi-core paths. The shipped
   library builds a translation unit per variant and dispatches between all
   of them at run time, which one file cannot do.</p>
<p class="fine">Built from <code>{commit}</code> on {date}. Compiler Explorer
   stores the source with the link, so this page is repointed whenever the
   library changes; the snapshot above is from that commit, not from the
   current tip. <a href="{repo}">The source it runs</a>.</p>
"""


def commit() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return "unknown"


def shorten(source: pathlib.Path, libs: list) -> str:
    cmd = [sys.executable, str(REPO / "tools/godbolt-link.py"), str(source)]
    for lib in libs:
        cmd += ["--lib", lib]
    return subprocess.run(cmd, capture_output=True, text=True,
                          check=True).stdout.strip()


def summarise(readme: str, fallback: str):
    """The heading of a README, and the paragraph under it."""
    lines = readme.split("\n")
    title, para, seen = fallback, [], False
    for line in lines:
        if not seen:
            if line.startswith("# "):
                title, seen = line[2:].strip(), True
            continue
        if not line.strip():
            if para:
                break
            continue
        para.append(line.strip())
    return title, " ".join(" ".join(para).split())


def targets():
    """The root demo, then one entry per example directory."""
    out = [ROOT]
    for d in sorted((REPO / "examples").iterdir()):
        readme, main = d / "README.md", d / "main.cpp"
        if not (d.is_dir() and readme.is_file() and main.is_file()):
            continue
        title, blurb = summarise(readme.read_text(), d.name)
        out.append({
            "slug": d.name,
            "title": title,
            "source": str(main.relative_to(REPO)),
            "libs": ["beman_execution:trunk"] if d.name == NEEDS_EXECUTION else [],
            "blurb": blurb,
        })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--no-link", action="store_true",
                    help="never contact godbolt.org; reuse the known links")
    ap.add_argument("--force", action="store_true",
                    help="mint a new link even when the source is unchanged")
    a = ap.parse_args()

    a.output.mkdir(parents=True, exist_ok=True)
    manifest_path = a.output / "links.json"
    manifest = {}
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text())

    # The library half is the same for every example; build it once.
    lib = a.output / ".lib.cpp"
    subprocess.run([sys.executable, str(REPO / "tools/amalgamate.py"),
                    "--no-demo", "-o", str(lib)],
                   check=True, capture_output=True)
    library = lib.read_text()
    lib.unlink()

    stamp, today = commit(), datetime.date.today().isoformat()
    for t in targets():
        # The example's own includes of the library are dropped: it is
        # already above them in the file.
        body = (REPO / t["source"]).read_text()
        body = re.sub(r"^#include <blake3pp/[^>]+>\n", "", body, flags=re.M)
        source = library + "\n" + body
        sha = hashlib.sha256(source.encode()).hexdigest()

        known = manifest.get(t["slug"] or "root", {})
        if known.get("sha") == sha and not a.force:
            url = known["url"]
            note = "unchanged"
        elif a.no_link:
            url = known.get("url", "https://godbolt.org/")
            note = "stale, offline" if known else "no link yet, offline"
        else:
            tmp = a.output / ".full.cpp"
            tmp.write_text(source)
            url = shorten(tmp, t["libs"])
            tmp.unlink()
            note = "new link"
        manifest[t["slug"] or "root"] = {"url": url, "sha": sha}

        page = a.output / t["slug"] / "index.html" if t["slug"] \
            else a.output / "index.html"
        page.parent.mkdir(parents=True, exist_ok=True)
        repo_url = ("https://github.com/pysco68/blake3pp/blob/main/" + t["source"])
        page.write_text(PAGE.format(
            title=html.escape(t["title"]),
            url=html.escape(url, quote=True),
            blurb=html.escape(t["blurb"]),
            commit=html.escape(stamp), date=today,
            repo=html.escape(repo_url, quote=True)))
        print(f"  {t['slug'] or '/':<24} {note:<16} {url}")

    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
