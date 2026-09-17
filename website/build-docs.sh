#!/usr/bin/env bash
# Build one version of the documentation site.
#
# Three inputs become one directory: the markdown in docs/, the README as
# the landing page, and the `///` comments in the public headers by way of
# Doxygen's XML. website/build.sh calls this once per published version.
#
# Nothing is generated into the source tree: the three are staged into
# build/site-src, where the links can be rewritten for a site whose root
# is docs/ without breaking the same links when read on GitHub.
#
#   website/build-docs.sh --out _site/main
#   website/build-docs.sh --out _site/v0.1.0 --links /tmp/links.json
set -euo pipefail

cd "$(dirname "$0")/.."
out=""
links=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out)   out=$2; shift 2 ;;
    --links) links=$2; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-docs: unknown option '$1'" >&2; exit 2 ;;
  esac
done
[ -n "${out}" ] || { echo "build-docs: --out is required" >&2; exit 2; }
case "${out}" in /*) ;; *) out="${PWD}/${out}" ;; esac
if [ -n "${links}" ]; then
  case "${links}" in /*) ;; *) links="${PWD}/${links}" ;; esac
fi

# mkdocs is a Python application, not a system package; keep it in a venv
# beside the checkout so a contributor needs no global installs.
venv=${BLAKE3PP_DOCS_VENV:-${PWD}/.venv-docs}
if [ ! -x "${venv}/bin/mkdocs" ]; then
  echo "-- creating ${venv}"
  python3 -m venv "${venv}"
  "${venv}/bin/pip" install -q -r website/requirements.txt
fi

command -v doxygen > /dev/null || {
  echo "build-docs: doxygen is not installed (apt install doxygen)" >&2; exit 1; }

src=build/site-src
rm -rf "${src}" build/doxygen
mkdir -p "${src}" build/doxygen
cp docs/*.md "${src}/"

# The reference pages, straight from the headers. Doxygen writes XML only;
# none of its HTML is published.
echo "-- extracting the header documentation"
doxygen - > /dev/null <<DOXY
$(cat website/Doxyfile)
XML_OUTPUT = ${PWD}/build/doxygen/xml
DOXY
python3 website/doxygen-to-md.py --xml build/doxygen/xml --out "${src}/reference"

# docs/index.md is the site's landing page and the README is not published:
# the two have different jobs. The documents' back-links to the README are
# repointed at that landing page.
echo "-- staging the documents"
python3 - "${src}" "${links}" <<'PY'
import json
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1])
BLOB = "https://github.com/pysco68/blake3pp/blob/main/"

# The READMEs point at the redirect pages at the site root, which is what
# a link committed to git has to do. This site is rebuilt for every
# published version, so it carries each version's own Compiler Explorer
# link instead, and a link standing alone in its paragraph becomes a
# button.
TRY = re.compile(r"\]\(https://pysco68\.github\.io/blake3pp/try/([\w.-]*)/?\)")
ALONE = re.compile(r"^\s*\[[^\]]+\]\(https://pysco68\.github\.io/blake3pp/try/[\w.-]*/?\)\s*$")
# Compiler Explorer opens in a tab of its own: following one of these is
# trying the example, not leaving the documentation behind.
NEW_TAB = "target=_blank rel=noopener"
BUTTON = "{ .md-button .md-button--primary " + NEW_TAB + " }"
INLINE = "{ " + NEW_TAB + " }"
links = json.loads(pathlib.Path(sys.argv[2]).read_text()) if len(sys.argv) > 2 \
    and sys.argv[2] else {}


def retarget(text: str) -> str:
    out = []
    for line in text.split("\n"):
        if not TRY.search(line):
            out.append(line)
            continue
        button = bool(ALONE.match(line))

        def swap(m, button=button):
            url = links.get(m.group(1) or "root")
            link = m.group(0) if url is None else f"]({url})"
            # A button carries the attributes for the whole line instead.
            return link if button else link + INLINE

        line = TRY.sub(swap, line)
        if button and not line.rstrip().endswith("}"):
            line = line.rstrip() + BUTTON
        out.append(line)
    return "\n".join(out)

for page in src.glob("*.md"):
    text = page.read_text().replace("(../README.md", "(index.md")
    # Whatever is still relative is a repository path, not a page here.
    text = re.sub(r"\]\((?!https?:|#|\w[\w.-]*\.md|\w[\w.-]*/)([^)]+)\)",
                  rf"]({BLOB}\1)", text)
    page.write_text(retarget(text))

# The examples become pages of their own, each README followed by the
# program it describes, so the site carries the code and not only a
# pointer to it.
examples = src / "examples"
examples.mkdir(exist_ok=True)
for d in sorted(pathlib.Path("examples").iterdir()):
    readme, main = d / "README.md", d / "main.cpp"
    if not (d.is_dir() and readme.is_file() and main.is_file()):
        continue
    text = readme.read_text()
    text += "\n## The whole program\n\n```cpp\n" + main.read_text().strip() + "\n```\n"
    # Sibling examples are pages here, and docs/ is one level up.
    text = re.sub(r"\]\(\.\./(\d[\w-]+)/\)", r"](\1.md)", text)
    text = text.replace("](../../docs/", "](../")
    (examples / (d.name + ".md")).write_text(retarget(text))

listing = pathlib.Path("examples/README.md")
if listing.is_file():
    text = re.sub(r"\]\((\d[\w-]+)/\)", r"](\1.md)", listing.read_text())
    (examples / "index.md").write_text(retarget(text))
PY

# --strict, and the status is propagated: a version that does not build
# must not be published half-rendered. The filter drops mkdocs' own INFO
# chatter and the upstream banner about a future MkDocs 2.0, neither of
# which says anything about this build.
echo "-- mkdocs build -> ${out}"
log=$(mktemp); trap 'rm -f "${log}"' EXIT
status=0
"${venv}/bin/mkdocs" build -f website/mkdocs.yml --strict --site-dir "${out}" > "${log}" 2>&1 || status=$?
grep -vE "^INFO|^[[:space:]]*$|Material for MkDocs team|MkDocs 2\.0|plugin system|theming system|migration path|contribution model|Currently unlicensed|squidfunk\.github\.io|full analysis|^.\[3[0-9]m" "${log}" >&2 || true
exit ${status}
