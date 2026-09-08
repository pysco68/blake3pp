#!/usr/bin/env python3
"""Emit the tipi cmake-re environment files next to the toolchain files.

    python3 tools/gen-environments.py            # write, tag from HEAD's docker tree
    python3 tools/gen-environments.py --tag latest
    python3 tools/gen-environments.py --check    # exit 1 if anything is stale

cmake-re pairs a toolchain file with an OS image, and the toolchain's
folder is the unit it works with: everything at the toolchain file's level
is copied into the environment and hashed into its identity. So each
toolchain lives in its own folder, cmake/toolchains/<name>/, holding
<name>.cmake, <name>.pkr.js (the environment: the image the toolchain
builds in, pinned to the content tag tools/toolchain-image-tag.sh computes
for the checkout, the hard reference tipi's docs ask for) and, only where
the toolchain includes sibling files (../common.cmake, or the base
toolchain of a hand-written variant), a <name>.layers.json that pulls
them in ("../" navigation is honoured; a parent-level layers file is not
composed into a child's environment, measured on v0.0.87).
That pairing is what lets cmake-re run the same build remotely
(--remote, RBE) instead of on this host.

The immutable form is not written here: once cmake-re has resolved the
image (locally or by pulling it) it writes <name>.container.lock beside
the toolchain, with the registry manifest digest and the environment
hash, and marks it for version control. Those lock files are the digest
pins to commit, produced against the real images rather than guessed.

The preset-to-image patterns mirror tools/tc's; keep the two in step.
"""

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLCHAINS = os.path.join(REPO, "cmake", "toolchains")
REGISTRY = os.environ.get("BLAKE3PP_TC_REGISTRY", "ghcr.io/pysco68/blake3pp")
# How an image name is formed from the registry and the image's short name.
# GHCR nests repositories (registry/toolchain-zig); Docker Hub allows only
# <namespace>/<name>, so a public mirror there is spelled
#   BLAKE3PP_TC_IMAGE_TEMPLATE="docker.io/<ns>/blake3pp-toolchain-{image}"
IMAGE_TEMPLATE = os.environ.get("BLAKE3PP_TC_IMAGE_TEMPLATE", "{registry}/toolchain-{image}")

# Same table as tools/tc (image_for_preset), preset glob -> image name.
IMAGE_FOR = [
    ("linux-gcc14-*", "gcc14"),
    ("linux-gcc16-*", "gcc16"),
    ("linux-clang18-*", "clang18"),
    ("linux-clang20-*", "clang20"),
    ("linux-clang22-*", "clang22"),
    ("linux-arm64-gcc15-*", "arm64-gcc15"),
    ("linux-riscv64-gcc15-*", "riscv64-gcc15"),
    ("linux-ppc64le-gcc15-*", "ppc64le-gcc15"),
    ("linux-s390x-gcc15-*", "s390x-gcc15"),
    ("*zigmusl*", "zig"),
    ("wasm32-*", "emscripten"),
]
# Not containerized: no environment (windows and macos build natively).
SKIP = ["windows-*", "macos-*"]


def image_for(name):
    for pattern, image in IMAGE_FOR:
        if fnmatch.fnmatch(name, pattern):
            return image
    return None


def content_tag():
    out = subprocess.run(["bash", os.path.join(REPO, "tools", "toolchain-image-tag.sh")],
                         capture_output=True, text=True, check=True).stdout.strip()
    return f"tree-{out}"


def environment(name, tag):
    return json.dumps({
        "variables": {},
        "builders": [{
            "type": "docker",
            "image": IMAGE_TEMPLATE.format(registry=REGISTRY, image=image_for(name)) + f":{tag}",
            "commit": True,
        }],
    }, indent=2) + "\n"


def layers(name):
    """The layers file, or None when the folder's own files are the environment.

    Every include() of a sibling path (../common.cmake for the generated
    toolchains, ../<base>/<base>.cmake for the hand-written variants) is a
    layer the environment must carry."""
    with open(os.path.join(TOOLCHAINS, name, f"{name}.cmake")) as fh:
        found = re.findall(r'include\("\$\{CMAKE_CURRENT_LIST_DIR\}/(\.\./[^"]+)"\)', fh.read())
    if found:
        return json.dumps(sorted(set(found)), indent=2) + "\n"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", help="image tag (default: tree-<hash> of HEAD's docker tree)")
    ap.add_argument("--check", action="store_true", help="report stale files, write nothing")
    args = ap.parse_args()
    tag = args.tag or content_tag()

    stale = []
    for name in sorted(os.listdir(TOOLCHAINS)):
        if not os.path.isfile(os.path.join(TOOLCHAINS, name, f"{name}.cmake")):
            continue  # common.cmake and friends live at the top level
        if any(fnmatch.fnmatch(name, s) for s in SKIP):
            continue
        if image_for(name) is None:
            sys.exit(f"gen-environments: no image pattern for {name}; extend IMAGE_FOR (and tools/tc)")
        wanted = {
            os.path.join(TOOLCHAINS, name, f"{name}.pkr.js"): environment(name, tag),
            os.path.join(TOOLCHAINS, name, f"{name}.layers.json"): layers(name),
        }
        for path, content in wanted.items():
            current = open(path).read() if os.path.exists(path) else None
            if current == content:
                continue
            stale.append(os.path.relpath(path, REPO))
            if args.check:
                continue
            if content is None:
                os.remove(path)
            else:
                with open(path, "w") as fh:
                    fh.write(content)
    if args.check:
        for path in stale:
            print(f"stale: {path}")
        return 1 if stale else 0
    print(f"{len(stale)} file(s) written, image tag {tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
