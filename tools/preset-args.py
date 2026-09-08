#!/usr/bin/env python3
"""Print the configure arguments a CMake preset stands for.

cmake-re knows nothing about presets, so anything driving it (the rbe
workflow, tools/make-release.sh in its cmake-re mode) needs the preset
unrolled into plain arguments: generator, toolchain file, binary
directory and cache variables, inherits resolved, ${sourceDir} taken as
the repository root. One line, shell-quoted, ready for $(...).

    python3 tools/preset-args.py linux-riscv64-zigmusl-cxx23-static
    -> -G Ninja -B build/linux-riscv64-zigmusl-cxx23-static
       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/.../linux-riscv64-zigmusl-cxx23.cmake
       -DCMAKE_BUILD_TYPE=Release -DBLAKE3PP_XTHEAD_KERNEL=ON

    python3 tools/preset-args.py --binary-dir <preset>   # just the build dir
"""
import argparse
import json
import os
import shlex
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_presets(path, seen=None):
    seen = seen or set()
    if path in seen:
        return {}
    seen.add(path)
    with open(path) as f:
        doc = json.load(f)
    presets = {}
    for inc in doc.get("include", []):
        presets.update(load_presets(os.path.join(os.path.dirname(path), inc), seen))
    for p in doc.get("configurePresets", []):
        presets[p["name"]] = p
    return presets


def resolve(presets, name):
    """The preset with its inherits folded in (nearer definitions win)."""
    p = presets.get(name)
    if p is None:
        sys.exit(f"preset-args: no configure preset named '{name}'")
    parents = p.get("inherits", [])
    if isinstance(parents, str):
        parents = [parents]
    merged = {"cacheVariables": {}}
    for parent in parents:
        base = resolve(presets, parent)
        merged["cacheVariables"].update(base.pop("cacheVariables"))
        merged.update(base)
    merged["cacheVariables"].update(p.get("cacheVariables", {}))
    for key in ("generator", "toolchainFile", "binaryDir"):
        if key in p:
            merged[key] = p[key]
    return merged


def expand(value):
    # Paths come out relative to the repo root: the callers run there,
    # and cmake-re copies the toolchain file into its own environment
    # directory, where an absolute host path is one more thing to mirror.
    return value.replace("${sourceDir}/", "").replace("${sourceDir}", ".")


def cache_value(v):
    if isinstance(v, dict):
        return f"{v['value']}" if "type" not in v else f"{v['value']}"
    if isinstance(v, bool):
        return "ON" if v else "OFF"
    return str(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("preset")
    ap.add_argument("--binary-dir", action="store_true",
                    help="print only the preset's binary directory")
    args = ap.parse_args()
    p = resolve(load_presets(os.path.join(REPO, "CMakePresets.json")), args.preset)
    binary_dir = expand(p.get("binaryDir", f"build/{args.preset}"))
    if args.binary_dir:
        print(binary_dir)
        return
    words = ["-G", p.get("generator", "Ninja"), "-B", binary_dir]
    if "toolchainFile" in p:
        words.append(f"-DCMAKE_TOOLCHAIN_FILE={expand(p['toolchainFile'])}")
    for k, v in p["cacheVariables"].items():
        words.append(f"-D{k}={expand(cache_value(v))}")
    print(" ".join(shlex.quote(w) for w in words))


if __name__ == "__main__":
    main()
