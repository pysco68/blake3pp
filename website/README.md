# website/

Everything that builds <https://pysco68.github.io/blake3pp>: the MkDocs
configuration, the scripts that assemble each published version, and the
Compiler Explorer machinery behind the "try it" links. None of it ships
with the library, and all of it runs from the repository root.

| | |
|---|---|
| `build.sh` | The entry point: every version, the try pages, and the version dropdown. |
| `build-docs.sh` | One version of the site, staged and rendered. |
| `mkdocs.yml` | The MkDocs configuration, Material theme and navigation. |
| `requirements.txt` | The Python side, installed into a `.venv-docs` virtualenv. |
| `doxygen-to-md.py` | Doxygen's XML into the reference pages. |
| `Doxyfile` | The Doxygen configuration, XML only: no Doxygen HTML is published. |
| `build-try.py` | A "try it on Compiler Explorer" page per example, minting a link when its source changed. |
| `amalgamate.py` | The library concatenated into one translation unit, which is the only thing Compiler Explorer can run. Comments are stripped; the banner it writes is the one that stays. |
| `amalgam-demo.inc` | The program appended to that file when no other one is given. |
| `godbolt-link.py` | Shorten a source file into a Compiler Explorer link. |
| `godbolt-check.py` | Compile and run a source file on Compiler Explorer and report, for checking a snapshot before its link is published. |

The content is elsewhere: `docs/` holds the markdown, `examples/` the
programs, and the public headers their own doc comments. `_site/` is the
output and is not tracked.

## The documentation site

`build.sh` builds what `pysco68.github.io/blake3pp` serves. CI runs the
same script.

    website/build.sh --no-link --serve        # localhost:8000

| Option | Effect |
| --- | --- |
| `--all` | Build every `v*` tag as well as `main`, and write the `versions.json` behind the version dropdown. |
| `--no-link` | Reuse the links already in the cache instead of generating any. An offline preview needs this. |
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

### Why beman.execution

Every link selects it, whether or not the example hashes across cores.
The library needs a sender/receiver implementation for its multi-core
half, and of the three it supports, beman.execution is the one Compiler
Explorer already installs: selecting it there is a checkbox, where
stdexec would be a build. It is header-only, so an example that never
touches a scheduler pays compile time and nothing else, and one that does
gets `get_parallel_scheduler()` without the reader working out what to
add.

Leaving it out is a supported configuration. The amalgamation detects the
header; without it the multi-core entry points are not declared, and
`execution_provider()` answers `none`. That is what `07-which-kernel`
prints if you clear the library list.
