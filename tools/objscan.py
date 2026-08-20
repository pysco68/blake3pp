#!/usr/bin/env python3
"""Look inside a built binary: which instructions it carries, and where.

The questions a fat binary keeps raising, answered from the disassembly
rather than from the compiler's flags: did the vector kernel actually
vectorize, did the facade inline or leave a call soup behind, which
functions use an instruction the dispatch verdict does not gate.

    tools/objscan.py mnemonics BIN [--all] [--per-function] [-x RE]
        Histogram of the vector mnemonics in BIN (--all: every mnemonic),
        optionally per function; -x excludes symbols matching RE.
    tools/objscan.py find BIN REGEX [-x RE] [--fail]
        Functions containing a mnemonic matching REGEX, with counts;
        --fail exits 1 when any is found ("no z14/z15 vector instruction
        outside the vxe kernel" is
        `find BIN '^(vlbr|vnx|vnn|voc|vmsl|vbperm|vfmin|vfmax)' -x vxe --fail`).
    tools/objscan.py disasm BIN SYMBOL
        Disassembly of every function whose demangled name matches the
        regular expression SYMBOL.
    tools/objscan.py quality BIN [--hot RE] [--allow RE]
        How good the code generation is, per hot function: size, vector
        density, loops (backward branches: an unrolled kernel has the
        block loop and little else), calls that survived inlining (with
        their callees; anything outside --allow is an inlining failure)
        and vector traffic through the stack frame (spills).

Runs on Linux, macOS and Windows: ELF, Mach-O and PE/COFF are recognised
from their headers. The disassembler is the target-prefixed GNU binutils
when present (aarch64-linux-gnu-objdump and friends, for the cross
lanes), llvm-objdump otherwise (Xcode's objdump, the LLVM install on the
Windows runners), dumpbin or otool as last resorts, or --objdump to name
one. Function names come demangled from the disassembler; Mach-O keeps
its leading underscore and the MSVC demangler spells the anonymous
namespace its own way, so patterns match on `kern::<ns>::` rather than
on whole names.
"""
import argparse
import collections
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

# ---------------------------------------------------------------- formats

ELF_MACHINES = {62: "x86_64", 183: "aarch64"}
PE_MACHINES = {0x8664: "x86_64", 0xAA64: "aarch64"}
MACHO_CPUTYPES = {0x01000007: "x86_64", 0x0100000C: "aarch64"}
BINUTILS_PREFIX = {"aarch64": "aarch64-linux-gnu-"}


def identify(path):
    """(arch, format) from the file header; format is elf, pe or macho."""
    with open(path, "rb") as fh:
        head = fh.read(64)
    if head[:4] == b"\x7fELF":
        endian = "<" if head[5] == 1 else ">"
        (machine,) = struct.unpack(endian + "H", head[18:20])
        if machine not in ELF_MACHINES:
            sys.exit(f"objscan: unsupported ELF machine {machine} in {path}")
        return ELF_MACHINES[machine], "elf"
    if head[:2] == b"MZ":
        (pe_off,) = struct.unpack("<I", head[60:64])
        with open(path, "rb") as fh:
            fh.seek(pe_off)
            sig = fh.read(6)
        if sig[:4] != b"PE\0\0":
            sys.exit(f"objscan: {path} is not a PE image")
        (machine,) = struct.unpack("<H", sig[4:6])
        return PE_MACHINES.get(machine, f"pe-{machine:#x}"), "pe"
    if len(head) >= 20 and struct.unpack("<H", head[:2])[0] in PE_MACHINES:
        # a COFF object has no MZ stub: the machine word comes first
        return PE_MACHINES[struct.unpack("<H", head[:2])[0]], "pe"
    if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
        (cputype,) = struct.unpack("<I", head[4:8])
        return MACHO_CPUTYPES.get(cputype, f"macho-{cputype:#x}"), "macho"
    sys.exit(f"objscan: {path} is neither ELF, PE nor Mach-O (a wasm module has no objdump)")


# llvm-objdump decodes only its default CPU's instructions on these targets.
LLVM_FLAGS = {
}


def tool(arch, fmt, name, override=None):
    """The <name> (objdump, nm, addr2line) for this target, on PATH."""
    if override:
        return override
    candidates = []
    if fmt == "elf":
        candidates.append(BINUTILS_PREFIX.get(arch, "") + name)
    candidates += ["llvm-" + name, name]
    if fmt == "pe" and name == "objdump":
        candidates.append("dumpbin")
    if fmt == "macho" and name == "objdump":
        candidates.append("otool")
    for candidate in candidates:
        if shutil.which(candidate):
            return candidate
    sys.exit(f"objscan: no {name} for {arch}/{fmt} on PATH (tried {', '.join(candidates)})")


# ------------------------------------------------------------ disassembly

# GNU and llvm objdump:  "0000000000401000 <sym>:"  then  "  401000:\tmnemonic ops"
#                        relocations (-r) on their own line: "  401001: R_X86_64_PLT32 memcpy-0x4"
# dumpbin /DISASM:       "sym:"  then  "  0000000140001000: 48 83 EC 28  sub  rsp,28h"
# otool -tV:             "_sym:"  then  "0000000100003f5c\tsub\tsp, sp, #0x30"
FUNC = re.compile(r"^(?:[0-9a-fA-F]+ <(?P<gnu>.+)>|(?P<bare>[^\s<>][^\t]*)):$")
INSN = re.compile(r"^\s*([0-9a-fA-F]+):?\s+((?:[0-9A-F]{2} )*)\s*([a-z][\w.]*)\s*(.*)$")
RELOC = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:R_\w+|IMAGE_REL_\w+|\w+_RELOC_\w+)\s+(.+?)(?:[+-]0x[0-9a-fA-F]+)?\s*$")
REGISTER = re.compile(r"^(%?[re]?[abcd]x|%?r\d+[bwd]?|%?[re]?[sd]i|%?[re]?[sb]p|[xw]\d+|[astx]\d+|ra|sp|lr|ctr|%?r1[0-5]|zero)$")
TARGET = re.compile(r"(?:0x)?([0-9a-fA-F]+)(?:\s*<([^>]*)>)?\s*$")


class Insn:
    __slots__ = ("addr", "mnemonic", "operands", "callee")

    def __init__(self, addr, mnemonic, operands):
        self.addr, self.mnemonic, self.operands, self.callee = addr, mnemonic, operands, None


def disassemble(path, objdump, arch):
    """{function: [Insn]} in file order. Call targets are resolved from the
    operand's <symbol> or, in a relocatable object, from the relocation
    the disassembler prints after the instruction."""
    base = os.path.basename(objdump).lower()
    if base.startswith("dumpbin"):
        cmd = [objdump, "/DISASM", "/NOLOGO", path]
    elif base.startswith("otool"):
        cmd = [objdump, "-tV", path]
    else:
        cmd = [objdump, "-d", "-r", "--no-show-raw-insn", "-C", path]
        if base.startswith("llvm-objdump"):
            cmd[1:1] = LLVM_FLAGS.get(arch, [])
    out = subprocess.run(cmd, capture_output=True, text=True, errors="replace", check=True).stdout
    functions = collections.OrderedDict()
    current, last = None, None
    for line in out.splitlines():
        m = INSN.match(line)
        if m and current is not None:
            last = Insn(int(m.group(1), 16), m.group(3).lower(), m.group(4).strip())
            functions[current].append(last)
            continue
        m = FUNC.match(line)
        if m:
            current = m.group("gnu") or m.group("bare")
            functions.setdefault(current, [])
            last = None
            continue
        m = RELOC.match(line)
        if m and last is not None:
            last.callee = m.group(1)
    names = demangle({i.callee for fns in functions.values() for i in fns if i.callee})
    for fns in functions.values():
        for insn in fns:
            if insn.callee in names:
                insn.callee = names[insn.callee]
    return functions


def versioned(name):
    """A distro's versioned llvm tool (llvm-cxxfilt-22) when the plain name is absent."""
    for d in os.environ.get("PATH", "").split(os.pathsep):
        hits = sorted(glob.glob(os.path.join(d, name + "-[0-9]*")))
        if hits:
            return hits[-1]
    return None


def demangle(names):
    """{mangled: demangled} for the relocation symbols (the disassembler
    demangles only the names it prints itself)."""
    mangled = sorted(n for n in names if n.startswith(("_Z", "__Z", "?")))
    filt = shutil.which("llvm-cxxfilt") or versioned("llvm-cxxfilt") or shutil.which("c++filt")
    if not mangled or not filt:
        return {}
    out = subprocess.run([filt], input="\n".join(n[1:] if n.startswith("__Z") else n for n in mangled) + "\n",
                         capture_output=True, text=True).stdout.splitlines()
    return dict(zip(mangled, out)) if len(out) == len(mangled) else {}


def call_target(fn, insn):
    """The callee's name: the relocation symbol, the <symbol> in the
    operands unless it is an unresolved offset into the caller itself, or
    (indirect) for a register or memory target."""
    if insn.callee:
        return insn.callee
    m = TARGET.search(insn.operands)
    if m and m.group(2):
        sym = m.group(2)
        if sym.startswith(fn + "+"):
            return "(unresolved)"
        return re.sub(r"\+0x[0-9a-fA-F]+$", "", sym)
    if re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", insn.operands):
        return "0x" + insn.operands.removeprefix("0x")
    if re.fullmatch(r"[?_a-zA-Z@$.][\w?@$.]*", insn.operands) and not REGISTER.match(insn.operands):
        return insn.operands  # dumpbin names the callee without brackets
    return "(indirect)"


def branch_target(insn):
    m = TARGET.search(insn.operands)
    return int(m.group(1), 16) if m else None


# ---------------------------------------------------------- architectures

# Vector classification: (mnemonic regex, operand regex); either match
# counts: x86 and arm64 name their vector registers.
VECTOR = {
    "x86_64": (r"^v[a-z]", r"\b[xyz]mm\d+\b"),
    "aarch64": (None, r"\b[vzq]\d+\b|\bp\d+/[mz]"),
}
# Control flow inside a function (loops) and out of it (calls).
BRANCH = {
    "x86_64": r"^j",
    "aarch64": r"^(b(\.\w+)?|cbn?z|tbn?z)$",
}
CALL = {
    "x86_64": r"^call",
    "aarch64": r"^blr?$",
}
# A memory operand through the stack pointer (or the frame pointer).
STACK = {
    "x86_64": r"\(%r[sb]p\)|\[r[sb]p\b",
    "aarch64": r"\[(sp|x29)\b",
}


def is_vector(arch, insn):
    mn, op = VECTOR[arch]
    return (mn is not None and re.search(mn, insn.mnemonic) is not None) \
        or (op is not None and re.search(op, insn.operands) is not None)


def selected(functions, arch, everything, exclude):
    """(function, mnemonic) pairs after the vector filter and the symbol exclusion."""
    ex = re.compile(exclude) if exclude else None
    for fn, insns in functions.items():
        if ex and ex.search(fn):
            continue
        for insn in insns:
            if everything or is_vector(arch, insn):
                yield fn, insn.mnemonic


def measure(arch, fn, insns):
    """The quality figures of one function."""
    branch, call, stack = (re.compile(BRANCH[arch]), re.compile(CALL[arch]), re.compile(STACK[arch]))
    lo = insns[0].addr if insns else 0
    vector = loops = spills = 0
    calls = collections.Counter()
    indirect = []
    for i, insn in enumerate(insns):
        v = is_vector(arch, insn)
        vector += v
        if call.match(insn.mnemonic):
            target = call_target(fn, insn)
            calls[target] += 1
            if target == "(indirect)":
                indirect.append("; ".join(f"{p.mnemonic} {p.operands}" for p in insns[max(0, i - 3):i + 1]))
        elif branch.match(insn.mnemonic):
            t = branch_target(insn)
            if t is not None and lo <= t <= insn.addr:
                loops += 1
        if v and stack.search(insn.operands):
            spills += 1
    return {"insns": len(insns), "vector": vector, "loops": loops, "spills": spills, "calls": calls, "indirect": indirect}


# --------------------------------------------------------------- commands

def load(path, objdump_override):
    arch, fmt = identify(path)
    return arch, disassemble(path, tool(arch, fmt, "objdump", objdump_override), arch)


def cmd_mnemonics(args):
    arch, functions = load(args.binary, args.objdump)
    pairs = list(selected(functions, arch, args.all, args.exclude))
    if args.per_function:
        per = collections.defaultdict(collections.Counter)
        for fn, mn in pairs:
            per[fn][mn] += 1
        for fn, counter in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
            print(f"{sum(counter.values()):7d}  {fn}")
            for mn, n in counter.most_common():
                print(f"{'':7}    {n:6d} {mn}")
    else:
        counter = collections.Counter(mn for _, mn in pairs)
        for mn, n in counter.most_common():
            print(f"{n:7d} {mn}")
        kind = "instructions" if args.all else "vector instructions"
        print(f"{sum(counter.values())} {kind}, {len(counter)} distinct mnemonics, "
              f"{arch} ({args.binary})", file=sys.stderr)


def cmd_find(args):
    arch, functions = load(args.binary, args.objdump)
    pattern = re.compile(args.regex)
    ex = re.compile(args.exclude) if args.exclude else None
    hits = collections.OrderedDict()
    for fn, insns in functions.items():
        if ex and ex.search(fn):
            continue
        counter = collections.Counter(i.mnemonic for i in insns if pattern.search(i.mnemonic))
        if counter:
            hits[fn] = counter
    for fn, counter in hits.items():
        print(f"{sum(counter.values()):7d}  {fn}  ({', '.join(f'{mn} x{n}' for mn, n in counter.most_common())})")
    total = sum(sum(c.values()) for c in hits.values())
    print(f"{total} matching instructions in {len(hits)} functions", file=sys.stderr)
    if args.fail and hits:
        sys.exit(1)


def cmd_disasm(args):
    arch, functions = load(args.binary, args.objdump)
    pattern = re.compile(args.symbol)
    shown = 0
    for fn, insns in functions.items():
        if not pattern.search(fn):
            continue
        shown += 1
        print(f"{insns[0].addr:016x} <{fn}>:" if insns else f"<{fn}>: (empty)")
        for insn in insns:
            print(f"    {insn.addr:8x}:  {insn.mnemonic:<12} {insn.operands}")
        print()
    if not shown:
        sys.exit(f"objscan: no function matches /{args.symbol}/")


HOT = r"::(hash_many|hash_batch|xof_wide|compress_in_place|compress_xof)\("
ALLOW = (r"^_?(memcpy|memset|memmove|__stack_chk_fail|__chkstk|__security_check_cookie|__security_push_cookie|__security_pop_cookie|__GSHandlerCheck"
         r"|__asan_\w+|__hwasan_\w+|__ubsan_\w+|__tsan_\w+|__msan_\w+|__sanitizer_\w+|__gcov\w*|__llvm_\w+)$"
         r"|kern::\w+::.*(hash_many|hash_batch|xof_wide|xof_many|compress_in_place|compress_xof)\(")


def short(fn):
    """A function name without its parameter list, for tables."""
    if fn.startswith("("):
        return fn  # the (indirect) and (unresolved) markers
    fn = re.sub(r"\(anonymous namespace\)|`anonymous namespace'", "{anon}", fn)
    return re.sub(r"\(.*$", "", fn)


def quality_rows(arch, functions, hot, allow):
    hot_re, allow_re = re.compile(hot), re.compile(allow)
    for fn, insns in functions.items():
        if not hot_re.search(fn) or not insns:
            continue
        q = measure(arch, fn, insns)
        q["unexpected"] = collections.Counter({c: n for c, n in q["calls"].items() if not allow_re.search(c)})
        yield fn, q


def format_row(name, q):
    calls = ", ".join(f"{short(c)} x{n}" if n > 1 else short(c) for c, n in q["calls"].most_common())
    return (f"{name:<20.20} {q['insns']:6d} insns  {100 * q['vector'] // q['insns']:3d}% vector  "
            f"loops {q['loops']:3d}  stack-vector {q['spills']:4d}  calls {sum(q['calls'].values()):2d}"
            + (f" ({calls})" if calls else ""))


def cmd_quality(args):
    arch, functions = load(args.binary, args.objdump)
    rows = list(quality_rows(arch, functions, args.hot, args.allow))
    if not rows:
        sys.exit(f"objscan: no function matches /{args.hot}/")
    for fn, q in rows:
        print(format_row(short(fn).split("::")[-1], q))
        for c, n in q["unexpected"].most_common():
            print(f"{'':20}   unexpected call: {c} x{n}")


    """(variant, object) for every kernel object under a build tree
    (multi-config generators add a configuration directory)."""
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--objdump", help="disassembler to use instead of the one chosen from the file header")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("mnemonics"); p.add_argument("binary")
    p.add_argument("--all", action="store_true", help="every mnemonic, not only vector ones")
    p.add_argument("--per-function", action="store_true")
    p.add_argument("-x", "--exclude", metavar="RE", help="skip functions whose name matches")
    p.set_defaults(fn=cmd_mnemonics)
    p = sub.add_parser("find"); p.add_argument("binary"); p.add_argument("regex")
    p.add_argument("-x", "--exclude", metavar="RE"); p.add_argument("--fail", action="store_true")
    p.set_defaults(fn=cmd_find)
    p = sub.add_parser("disasm"); p.add_argument("binary"); p.add_argument("symbol")
    p.set_defaults(fn=cmd_disasm)
    p = sub.add_parser("quality"); p.add_argument("binary")
    p.add_argument("--hot", metavar="RE", default=HOT, help="functions to measure")
    p.add_argument("--allow", metavar="RE", default=ALLOW, help="callees that are not inlining failures")
    p.set_defaults(fn=cmd_quality)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
