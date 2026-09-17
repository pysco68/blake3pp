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
# only ever one implementation. Its Compiler Explorer links do come from
# the tag, so a reader of v0.1.0's documentation opens v0.1.0's code.
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

maps=$(mktemp -d)
trap 'rm -rf "${maps}"' EXIT
offline=""
[ "${link}" = 1 ] || offline="--no-link"

# The link cache lives in the checkout and is keyed by version, so a tag's
# links are minted once and then never again: its sources cannot change.
try() {                       # try <namespace> <emitted map> [extra args...]
  local ns=$1 map=$2; shift 2
  python3 tools/build-try.py --namespace "${ns}" --emit-map "${map}" \
    --manifest "${root}/site/try/links.json" ${offline} "$@"
}

# main is this checkout: it is what a contributor is previewing, and in CI
# it is the commit being published.
echo "== main"
# main shares its links with the redirect pages at the site root, which is
# where the README points: both describe the current tip.
echo "-- amalgamating"
python3 tools/amalgamate.py -o examples/blake3pp-single-file.cpp
try "" "${maps}/main.json" --output site/try
tools/build-docs.sh --out "${out}/main" --links "${maps}/main.json"
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
       "${root}/tools/build-docs.sh" "${root}/tools/build-try.py" \
       "${root}/tools/godbolt-link.py" "${tree}/tools/"
    cp "${root}/docs/requirements.txt" "${tree}/docs/"
    # A tag that predates a document the nav lists cannot build strictly;
    # it is skipped rather than failing the whole site.
    if ! (cd "${tree}" \
            && python3 tools/build-try.py --namespace "${tag}" \
                 --emit-map "${maps}/${tag}.json" \
                 --manifest "${root}/site/try/links.json" ${offline} \
            && tools/build-docs.sh --out "${out}/${tag}" \
                 --links "${maps}/${tag}.json"); then
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

cp -r site/try "${out}/try"
rm -f "${out}/try/links.json"

echo "-- built ${out}"
if [ "${serve}" = 1 ]; then
  echo "-- http://localhost:8000/  (try: http://localhost:8000/try/)"
  python3 -m http.server --directory "${out}" 8000
fi
