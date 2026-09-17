#!/usr/bin/env bash
# Build the published site: every documented version under one dropdown,
# plus the Compiler Explorer link the README points at.
#
# CI runs exactly this, so what you preview locally is what gets published.
#
#   tools/build-site.sh                 # this checkout only, as "main"
#   tools/build-site.sh --all           # main and every v* tag
#   tools/build-site.sh --no-link       # offline: keep the current CE link
#   tools/build-site.sh --serve         # ...and serve it on :8000
#
# Every version is built with the tooling from this checkout, not from the
# tag: a release documents its own headers, but how they are rendered is
# only ever one implementation.
set -euo pipefail

cd "$(dirname "$0")/.."
root=${PWD}
out=${root}/_site
link=1
serve=0
all=0
for arg in "$@"; do
  case "$arg" in
    --all)     all=1 ;;
    --no-link) link=0 ;;
    --serve)   serve=1 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-site: unknown option '$arg'" >&2; exit 2 ;;
  esac
done

rm -rf "${out}"
mkdir -p "${out}"
export BLAKE3PP_DOCS_VENV=${root}/.venv-docs

# main is this checkout: it is what a contributor is previewing, and in CI
# it is the commit being published.
echo "== main"
tools/build-docs.sh --out "${out}/main"
versions=("main")

if [ "${all}" = 1 ]; then
  work=$(mktemp -d); trap 'git -C "${root}" worktree prune > /dev/null 2>&1 || true; rm -rf "${work}"' EXIT
  # Newest first: the dropdown reads top-down and so does a reader.
  for tag in $(git -C "${root}" tag --list 'v*' --sort=-v:refname); do
    echo "== ${tag}"
    tree=${work}/${tag}
    git -C "${root}" worktree add --detach --quiet "${tree}" "${tag}"
    # The generator travels; the documented sources stay at the tag.
    cp "${root}/mkdocs.yml" "${tree}/mkdocs.yml"
    mkdir -p "${tree}/tools" "${tree}/docs"
    cp "${root}/tools/Doxyfile" "${root}/tools/doxygen-to-md.py" \
       "${root}/tools/build-docs.sh" "${tree}/tools/"
    cp "${root}/docs/requirements.txt" "${tree}/docs/"
    # A tag that predates a document the nav lists cannot build strictly;
    # it is skipped rather than failing the whole site.
    if ! (cd "${tree}" && tools/build-docs.sh --out "${out}/${tag}"); then
      echo "-- ${tag} does not build with today's nav; skipped" >&2
      rm -rf "${out:?}/${tag}"
      continue
    fi
    versions+=("${tag}")
  done
fi

# The newest release is what an arriving reader should land on; with no
# release yet, that is main.
default=main
for v in "${versions[@]}"; do
  case "$v" in v*) default=$v; break ;; esac
done
cp -r "${out}/${default}" "${out}/latest"

# versions.json in the format Material's version selector reads.
python3 - "${out}" "${default}" "${versions[@]}" <<'PY'
import json
import pathlib
import sys

out, default, names = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
# Releases first, newest to oldest, then the development branch.
ordered = [n for n in names if n != "main"] + [n for n in names if n == "main"]
entries = [{"version": n,
            "title": "main (development)" if n == "main" else n,
            "aliases": ["latest"] if n == default else []}
           for n in ordered]
(out / "versions.json").write_text(json.dumps(entries, indent=2) + "\n")
print(f"-- versions: {', '.join(n for n in ordered)} (latest -> {default})")
PY

cat > "${out}/index.html" <<HTML
<!doctype html>
<meta charset="utf-8">
<title>blake3pp</title>
<meta http-equiv="refresh" content="0; url=latest/">
<link rel="canonical" href="latest/">
<p>Taking you to <a href="latest/">the documentation</a>.</p>
HTML

# The Compiler Explorer link. It embeds the source it was made from and
# their compile nodes have no network, so it is regenerated from this
# checkout and lives at the site root: one link, always the newest.
tmp=$(mktemp -d)
echo "-- amalgamating"
python3 tools/amalgamate.py -o examples/blake3pp-single-file.cpp
python3 tools/amalgamate.py --no-demo -o "${tmp}/lib.cpp" > /dev/null
cat "${tmp}/lib.cpp" examples/parallel-demo.cpp > "${tmp}/full.cpp"

if [ "${link}" = 1 ]; then
  echo "-- shortening on godbolt.org"
  url=$(python3 tools/godbolt-link.py "${tmp}/full.cpp" --lib beman_execution:trunk)
else
  # Offline preview: keep whatever the checked-in page already points at.
  url=$(sed -n 's/.*rel="canonical" href="\([^"]*\)".*/\1/p' site/try/index.html 2>/dev/null || true)
  url=${url:-https://godbolt.org/}
  echo "-- offline: keeping ${url}"
fi
rm -rf "${tmp}"

python3 tools/make-try-page.py --url "${url}" \
  --commit "$(git -C "${root}" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
  --output site/try/index.html
mkdir -p "${out}/try"
cp site/try/index.html "${out}/try/index.html"

echo "-- built ${out}"
if [ "${serve}" = 1 ]; then
  echo "-- http://localhost:8000/  (try: http://localhost:8000/try/)"
  python3 -m http.server --directory "${out}" 8000
fi
