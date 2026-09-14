#!/usr/bin/env python3
"""Look inside a built binary: which instructions it carries, and where.

The questions a fat binary keeps raising, answered from the disassembly
rather than from the compiler's flags: did the vector kernel actually
vectorize, did the facade inline or leave a call soup behind, which
functions use an instruction the dispatch verdict does not gate, whose
address is that in the emulator's trace.

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
    tools/objscan.py diff A B [--all] [--limit N]
        Mnemonics used in A but not in B, with the functions of A that
        use them. Comparing a binary that crashes under an emulator
        configuration with one that survives it names the instruction.
    tools/objscan.py resolve BIN ADDR [--bias HEX]
        The symbol and source line at ADDR; --bias subtracts the load
        address of a position-independent executable, as reported by a
        qemu trace or a core.
    tools/objscan.py audit BUILD_DIR [--binary BIN] [--rules FILE]
        The CI assertion, driven by tools/kernel-audit.json: every kernel
        object under BUILD_DIR contains the instruction class its variant
        promises and nothing above it, its hot functions pass the quality
        thresholds, and (with --binary) the linked binary carries nothing
        above the baseline outside the kernels. Exit 1 on any failure.

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
import json
import os
import re
import shutil
import struct
import subprocess
import sys

# ---------------------------------------------------------------- formats

ELF_MACHINES = {62: "x86_64", 183: "aarch64", 243: "riscv64", 21: "ppc64", 22: "s390x",
                8: "mips64"}
PE_MACHINES = {0x8664: "x86_64", 0xAA64: "aarch64"}
MACHO_CPUTYPES = {0x01000007: "x86_64", 0x0100000C: "aarch64"}
BINUTILS_PREFIX = {"aarch64": "aarch64-linux-gnu-", "riscv64": "riscv64-linux-gnu-",
                   "ppc64": "powerpc64le-linux-gnu-", "s390x": "s390x-linux-gnu-",
                   "mips64": "mips64el-linux-gnuabi64-"}


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
    "riscv64": ["--mattr=+v,+zvbb"],
    "ppc64": ["--mcpu=pwr10"],
    "s390x": ["--mcpu=z16"],
}


def tool(arch, fmt, name, override=None):
    """The <name> (objdump, nm, addr2line) for this target, on PATH."""
    if override:
        return override
    candidates = []
    if fmt == "elf":
        candidates.append(BINUTILS_PREFIX.get(arch, "") + name)
    # The versioned llvm tool comes before the unprefixed one: a distro
    # that ships only llvm-objdump-22 still has the right disassembler,
    # and falling through to the host objdump means a foreign target
    # cannot be decoded at all.
    candidates += ["llvm-" + name, versioned("llvm-" + name) or "", name]
    if fmt == "pe" and name == "objdump":
        candidates.append("dumpbin")
    if fmt == "macho" and name == "objdump":
        candidates.append("otool")
    for candidate in candidates:
        if candidate and shutil.which(candidate):
            return candidate
    if fmt == "pe" and name == "objdump":
        found = msvc_tool("dumpbin")
        if found:
            return found
    sys.exit(f"objscan: no {name} for {arch}/{fmt} on PATH (tried {', '.join(c for c in candidates if c)})")


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
    run = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if run.returncode:
        # Usually a host disassembler handed a foreign object: binutils is
        # built per target, and the generic objdump refuses what it cannot
        # decode. Name the tool and the target instead of a traceback.
        sys.exit(f"objscan: {os.path.basename(objdump)} failed on {path} ({arch}): "
                 + (run.stderr.strip().splitlines() or ["exit " + str(run.returncode)])[0])
    out = run.stdout
    functions = collections.OrderedDict()
    current, last = None, None
    for line in out.splitlines():
        m = INSN.match(line)
        if m and current is not None:
            last = Insn(int(m.group(1), 16), m.group(3).lower(), m.group(4).strip())
            functions[current].append(last)
            continue
        m = FUNC.match(line)
        if m and not line.startswith("Disassembly of section"):
            name = m.group("gnu") or m.group("bare")
            # Local labels (RISC-V keeps .L* for relaxation, llvm's Mach-O
            # writer emits ltmp*) are not function boundaries.
            if current is None or not re.match(r"\.L|ltmp\d+$|\$[xd]$", name):
                current = name
                functions.setdefault(current, [])
            last = None
            continue
        m = RELOC.match(line)
        if m and last is not None and not m.group(1).startswith("*"):  # *ABS* is R_RISCV_RELAX's
            last.callee = m.group(1)
    # dumpbin prints its headings decorated (llvm-objdump's -C demangles
    # them) and names a call's target in the operands rather than on a
    # relocation line, so headings and those targets join the callees in
    # the demangling pass.
    is_call = re.compile(CALL[arch])
    names = demangle({i.callee for fns in functions.values() for i in fns if i.callee}
                     | {fn for fn in functions if fn.startswith(("?", "_Z", "__Z"))}
                     | {i.operands for fns in functions.values() for i in fns
                        if i.callee is None and i.operands.startswith("?") and is_call.match(i.mnemonic)})
    if any(fn in names for fn in functions):
        functions = collections.OrderedDict((names.get(fn, fn), insns) for fn, insns in functions.items())
    for fns in functions.values():
        prev = None
        for insn in fns:
            if insn.callee in names:
                insn.callee = names[insn.callee]
            elif insn.callee is None and is_call.match(insn.mnemonic) and insn.operands in names:
                insn.callee = names[insn.operands]
            # RISC-V's call is an auipc/jalr pair whose relocation sits on
            # the auipc; the jalr takes it over.
            if insn.callee is None and prev is not None and prev.mnemonic == "auipc" and prev.callee:
                insn.callee = prev.callee
            prev = insn
        # An AArch64 call through the GOT (Mach-O: adrp x16, sym@GOTPAGE;
        # ldr x16, [x16, sym@GOTPAGEOFF]; blr x16, how Apple's stack probe
        # ___chkstk_darwin is reached) carries its relocations on the
        # loads; the blr takes over the symbol of the load into its
        # register.
        for i, insn in enumerate(fns):
            if insn.mnemonic != "blr" or insn.callee is not None:
                continue
            reg = insn.operands.strip()
            for p in reversed(fns[max(0, i - 4):i]):
                if p.callee and p.mnemonic in ("ldr", "adrp") and p.operands.startswith(reg + ","):
                    insn.callee = p.callee
                    break
    return functions


def msvc_tool(name):
    """<name>.exe from the Visual Studio installation, for a shell that never
    ran vcvars. dumpbin and undname live only in the toolchain's bin
    directory, and a plain PowerShell step (as CI's audit is) has none of it
    on PATH."""
    if os.name != "nt":
        return None
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    run = subprocess.run([vswhere, "-products", "*", "-latest", "-find",
                          rf"VC\Tools\MSVC\**\{name}.exe"], capture_output=True, text=True)
    hits = [line.strip() for line in run.stdout.splitlines() if line.strip()]
    # An installation carries a toolchain per host architecture; picking one
    # built for another host yields a binary that cannot run.
    host = {"amd64": "hostx64", "arm64": "hostarm64", "x86": "hostx86"}.get(
        os.environ.get("PROCESSOR_ARCHITECTURE", "AMD64").lower(), "hostx64")
    return next((h for h in hits if host in h.lower()), hits[0] if hits else None)


def versioned(name):
    """A distro's versioned llvm tool (llvm-cxxfilt-22) when the plain name is absent."""
    for d in os.environ.get("PATH", "").split(os.pathsep):
        hits = sorted(glob.glob(os.path.join(d, name + "-[0-9]*")))
        if hits:
            return hits[-1]
    return None


def demangle(names):
    """{mangled: demangled} for the relocation symbols and the callees the
    disassembler prints undecorated: Itanium names through llvm-cxxfilt or
    c++filt, MSVC names (?...@@...) through llvm-undname or undname."""
    result = {}
    itanium = sorted(n for n in names if n.startswith(("_Z", "__Z")))
    filt = shutil.which("llvm-cxxfilt") or versioned("llvm-cxxfilt") or shutil.which("c++filt")
    if itanium and filt:
        out = subprocess.run([filt], input="\n".join(n[1:] if n.startswith("__Z") else n for n in itanium) + "\n",
                             capture_output=True, text=True).stdout.splitlines()
        if len(out) == len(itanium):
            result.update(zip(itanium, out))
    msvc = sorted(n for n in names if n.startswith("?"))
    undname = (shutil.which("llvm-undname") or versioned("llvm-undname")
               or shutil.which("undname") or msvc_tool("undname"))
    # Both take the names as arguments, so they go in batches: Windows caps
    # a command line at 32767 characters and a linked image has thousands of
    # symbols.
    for batch in batched(msvc, 8000) if undname else []:
        # llvm-undname prints the mangled name, the demangled one and a
        # blank line per symbol; undname prints 'is :- "..."'.
        out = subprocess.run([undname] + batch, capture_output=True, text=True).stdout
        found = re.findall(r'is :- "(.*)"', out) or [
            l for l in out.splitlines() if l and not l.startswith("?")]
        if len(found) == len(batch):
            result.update(zip(batch, found))
    return result


def batched(names, budget):
    """<names> in groups whose arguments fit one command line."""
    group, size = [], 0
    for name in names:
        if group and size + len(name) > budget:
            yield group
            group, size = [], 0
        group.append(name)
        size += len(name) + 1
    if group:
        yield group


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
    # A register name can be valid hex: RISC-V's a5 and s0, AArch64's x1d.
    # The register test comes first, so `jalr a5` is an indirect call and
    # not a call to address 0xa5.
    if REGISTER.match(insn.operands):
        return "(indirect)"
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
# counts. x86 and arm64 name their vector registers, the others prefix
# the mnemonic.
VECTOR = {
    "x86_64": (r"^v[a-z]", r"\b[xyz]mm\d+\b"),
    "aarch64": (None, r"\b[vzq]\d+\b|\bp\d+/[mz]"),
    "riscv64": (r"^(th\.)?v[a-z]", None),
    "ppc64": (r"^(v[a-z]|xx|xv|xs|lxv|stxv|lvx|stvx)", None),
    "s390x": (r"^[vw][a-z]", None),
    # MSA is the only thing on MIPS with $w registers; its mnemonics are
    # suffixed rather than prefixed and .d/.w collide with scalar FP.
    "mips64": (None, r"\$w\d+\b"),
}
# Control flow inside a function (loops) and out of it (calls).
BRANCH = {
    "x86_64": r"^j",
    "aarch64": r"^(b(\.\w+)?|cbn?z|tbn?z)$",
    "riscv64": r"^(c\.)?(b[a-z]*|j)$",
    "ppc64": r"^b(?!l$|lrl?$|ctrl?$|l[+-]$)",
    "s390x": r"^(j\w*|br|brcl?|brctg?|bc|c[lg]?[ir]?j\w*)$",
    "mips64": r"^(b(?!al$)[a-z]*|j)$",
}
CALL = {
    "x86_64": r"^call",
    "aarch64": r"^blr?$",
    "riscv64": r"^(jalr?|call)$",
    "ppc64": r"^(bl|bctrl|bl[+-])$",
    "s390x": r"^(brasl?|basr|bas)$",
    "mips64": r"^(jal|jalr|bal)$",
}
# A memory operand through the stack pointer (or the frame pointer).
STACK = {
    "x86_64": r"\(%r[sb]p\)|\[r[sb]p\b",
    "aarch64": r"\[(sp|x29)\b",
    "riscv64": r"\((sp|s0)\)",
    "ppc64": r"\(r?1\)|\(r?31\)",
    "s390x": r"\(%r1[15]\)",
    "mips64": r"\((sp|s8|fp)\)",
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
ALLOW = (r"^_?(memcpy|memset|memmove|__stack_chk_fail|__chkstk|__chkstk_darwin|__security_check_cookie|__security_push_cookie|__security_pop_cookie|__GSHandlerCheck"
         r"|__asan_\w+|__hwasan_\w+|__ubsan_\w+|__tsan_\w+|__msan_\w+|__sanitizer_\w+|__gcov\w*|__llvm_\w+)$"
         r"|kern::\w+::.*(hash_many|hash_batch|xof_wide|xof_many|compress_in_place|compress_xof)\("
         # MSVC keeps std::atomic<T>::load out of line; the kernels read the
         # transpose16 dial once per batch through it.
         r"|std::_Atomic_storage<.*>::load\(")

# The runtimes instrumented code calls (sanitizers, gcov, llvm profiling),
# plus the internals of a statically linked libgcov as they appear in a
# linked binary (libgcov-driver.c's static helpers).
RUNTIME = r"_?__(asan|hwasan|ubsan|tsan|msan|sanitizer|gcov|llvm)_|^_?(gcov_|mangle_path$)"
# GCC's outline atomics on AArch64 (-moutline-atomics, the default): the
# LSE-or-fallback helpers a __atomic read-modify-write becomes below armv8.1.
OUTLINE_ATOMICS = r"^__aarch64_(cas|swp|ldadd|ldclr|ldeor|ldset)\d+_(relax|acq|rel|acq_rel|sync)$"


def short(fn):
    """A function name without its parameter list, for tables."""
    if fn.startswith("("):
        return fn  # the (indirect) and (unresolved) markers
    fn = re.sub(r"\(anonymous namespace\)|`anonymous namespace'", "{anon}", fn)
    # undname spells the return type and calling convention first.
    fn = re.sub(r"^(?:public: |private: |protected: )?.*\b__(?:cdecl|vectorcall|fastcall|stdcall) ", "", fn)
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


def cmd_diff(args):
    arch_a, fa = load(args.a, args.objdump)
    arch_b, fb = load(args.b, args.objdump)
    if arch_a != arch_b:
        sys.exit(f"objscan: {args.a} is {arch_a}, {args.b} is {arch_b}")
    only_a = {mn for _, mn in selected(fa, arch_a, args.all, None)} \
        - {mn for _, mn in selected(fb, arch_b, args.all, None)}
    if not only_a:
        print(f"no mnemonic in {args.a} that {args.b} lacks")
        return
    users = collections.defaultdict(collections.Counter)
    for fn, mn in selected(fa, arch_a, args.all, None):
        if mn in only_a:
            users[mn][fn] += 1
    for mn in sorted(only_a):
        print(mn)
        for fn, n in users[mn].most_common(args.limit):
            print(f"    {n:6d} {fn}")


def cmd_resolve(args):
    arch, fmt = identify(args.binary)
    addr = int(args.address, 16) - (int(args.bias, 16) if args.bias else 0)
    nm = subprocess.run([tool(arch, fmt, "nm", None), "-C", "-n", "--defined-only", args.binary],
                        capture_output=True, text=True, check=True).stdout
    best = None
    for line in nm.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3 or parts[1].lower() not in "tw":
            continue
        start = int(parts[0], 16)
        if start <= addr:
            best = (start, parts[2])
    if best:
        print(f"{addr:#x} = {best[1]} + {addr - best[0]:#x}")
    else:
        print(f"{addr:#x}: no text symbol at or before this address")
    a2l = subprocess.run([tool(arch, fmt, "addr2line", None), "-f", "-C", "-i", "-e", args.binary, f"{addr:#x}"],
                         capture_output=True, text=True).stdout.strip()
    if a2l and not a2l.startswith("??"):
        print(a2l)


# ------------------------------------------------------------------ audit

def rule_hits(insns, rule):
    """Instructions matching a rule: {"mnemonic": RE} and/or {"operand": RE},
    either sufficing unless "all" asks for both."""
    mn = re.compile(rule["mnemonic"]) if "mnemonic" in rule else None
    op = re.compile(rule["operand"]) if "operand" in rule else None
    both = rule.get("all", False)
    n = 0
    for insn in insns:
        a = mn is not None and mn.search(insn.mnemonic) is not None
        b = op is not None and op.search(insn.operands) is not None
        if (a and b) if both else (a or b):
            n += 1
    return n


def kernel_objects(build_dir):
    """(variant, object) for every kernel object under a build tree: the
    OBJECT-library ones (multi-config generators add a configuration
    directory) and the externally compiled ones."""
    found = []
    for obj in glob.glob(os.path.join(build_dir, "CMakeFiles", "blake3pp_kernel_*.dir", "**", "*kernel.cpp.o*"),
                         recursive=True):
        found.append((re.search(r"blake3pp_kernel_([^/\\]+)\.dir", obj).group(1), obj))
    # A streaming (sme) variant's entry TU, the mode switch around the
    # kernel, is audited on its own: it must hold no SVE the compiler could
    # execute outside streaming mode.
    for obj in glob.glob(os.path.join(build_dir, "CMakeFiles", "blake3pp_kernel_*.dir", "**", "*sme_entry.cpp.o*"),
                         recursive=True):
        found.append((re.search(r"blake3pp_kernel_([^/\\]+)\.dir", obj).group(1) + "-entry", obj))
    for obj in glob.glob(os.path.join(build_dir, "blake3pp_generated", "kernel_*.o*")):
        found.append((re.search(r"kernel_([^/\\]+)\.o", os.path.basename(obj)).group(1), obj))
    return sorted(found)


def verdict(ok):
    return "OK  " if ok else "FAIL"


def cmd_audit(args):
    rules_path = args.rules or os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernel-audit.json")
    with open(rules_path) as fh:
        rules = json.load(fh)
    objects = kernel_objects(args.build_dir)
    if not objects:
        sys.exit(f"objscan: no kernel objects under {args.build_dir}")
    quality = rules.get("quality", {})
    hot, allow = quality.get("hot", HOT), quality.get("allow_calls", ALLOW)
    failures = 0
    arch = None
    for variant, obj in objects:
        arch, functions = load(obj, args.objdump)
        insns = [i for fn in functions.values() for i in fn]
        per_arch = rules.get(arch, {})
        spec = next((s for s in per_arch.get("variants", []) if re.fullmatch(s["match"], variant)), None)
        if spec is None:
            print(f"  {variant:<12} {len(insns):6d} insns  (no rule for {variant} on {arch})")
            continue
        ok, notes = True, []
        req = spec.get("require")
        if req:
            n = rule_hits(insns, req)
            need = req.get("min", rules.get("min_vector_instructions", 500))
            good = n >= need
            ok &= good
            notes.append(f"{req['name']}: {n} ({'>=' if good else '<'} {need})")
        for forbid in spec.get("forbid", []):
            n = rule_hits(insns, forbid)
            ok &= n == 0
            notes.append(f"no {forbid['name']}: {n}")
        print(f"  {variant:<12} {len(insns):6d} insns  {verdict(ok)}  {'; '.join(notes)}")
        failures += not ok
        limits = {**quality, **spec.get("quality", {})}
        # Instrumented code is a different shape: a sanitizer's check blocks
        # branch back into the fast path by the hundred, and gcov's counter
        # bumps split every block, so the loop budget does not apply. A
        # sanitizer announces itself through the runtime it calls; gcov
        # calls nothing from the hot path (its counters are plain or atomic
        # adds), so the notes file it writes beside the object is the tell.
        # With -fprofile-update=atomic on the armv8.0 baseline those adds
        # are GCC's outline-atomics helpers, which nothing else in a kernel
        # has any business calling.
        gcov = os.path.exists(re.sub(r"\.o(bj)?$", ".gcno", obj))
        rows = list(quality_rows(arch, functions, hot, allow + "|" + OUTLINE_ATOMICS if gcov else allow))
        instrumented = gcov or any(re.match(RUNTIME, c) for _, q in rows for c in q["calls"])
        for fn, q in rows:
            # Instrumented code is not the code that ships, and both quality
            # measures read it wrong: the counters split every block, and a
            # tail call becomes a real call because the counter has to run
            # after it (the xthead kernel delegates its single-block entries
            # to the scalar table that way). The ISA rules above still hold,
            # which is what a coverage lane is there to check.
            good = instrumented or (not q["unexpected"] and
                                    q["loops"] <= limits.get("max_loops", 24))
            print(f"    {verdict(good)} {format_row(short(fn).split('::')[-1], q)}")
            for c, n in q["unexpected"].most_common():
                print(f"{'':11}unexpected call: {c} x{n}")
            for site in q["indirect"]:
                print(f"{'':11}indirect call site: {site}")
            failures += not good
        if instrumented:
            print(f"    {'':4} instrumented ({'gcov notes beside the object' if gcov else 'sanitizer or coverage runtime called'}): loop and call budgets not applied")
        if req and rows:
            # The unrolled core: the hot function carrying the most vector
            # instructions (the others may be drivers around it).
            widest = max(q["vector"] for _, q in rows)
            need = limits.get("min_vector", 500)
            print(f"    {verdict(widest >= need)} widest hot function: {widest} vector instructions ({'>=' if widest >= need else '<'} {need})")
            failures += widest < need
    if args.binary:
        arch, functions = load(args.binary, args.objdump)
        # A coverage build links libgcov, compiled at the distribution's
        # baseline (RVA23 on Ubuntu's riscv64, vector included), so the
        # profiling runtime is exempt from the outside-the-kernels check.
        runtime = re.compile(RUNTIME) if glob.glob(os.path.join(args.build_dir, "CMakeFiles", "**", "*.gcno"),
                                                    recursive=True) else None
        # Without a symbol table a linked image disassembles as one block per
        # section, so no instruction can be attributed to a kernel: a PE keeps
        # its symbols in the PDB, a stripped ELF has none. Say so instead of
        # reporting the whole section as a violation.
        checks = rules.get(arch, {}).get("binary", [])
        if checks and functions and all(fn.startswith(".") for fn in functions):
            print(f"  {os.path.basename(args.binary):<12} SKIP  no function symbols: "
                  "the outside-the-kernels checks need a symbolized binary")
            checks = []
        for check in checks:
            outside = re.compile(check["outside"])
            hits = collections.Counter()
            for fn, insns in functions.items():
                if not outside.search(fn) and not (runtime and runtime.match(fn)):
                    n = rule_hits(insns, check)
                    if n:
                        hits[fn] += n
            failures += bool(hits)
            print(f"  {os.path.basename(args.binary):<12} {verdict(not hits)}  no {check['name']} outside /{check['outside']}/"
                  + ("" if not hits else ": " + ", ".join(f"{short(fn)} x{n}" for fn, n in hits.most_common(5))))
    print(f"kernel audit: {args.build_dir} ({arch}): {'FAILED, ' + str(failures) + ' finding(s)' if failures else 'OK'}")
    if failures:
        sys.exit(1)


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
    p = sub.add_parser("diff"); p.add_argument("a"); p.add_argument("b")
    p.add_argument("--all", action="store_true"); p.add_argument("--limit", type=int, default=5)
    p.set_defaults(fn=cmd_diff)
    p = sub.add_parser("resolve"); p.add_argument("binary"); p.add_argument("address")
    p.add_argument("--bias", help="load address to subtract (PIE)")
    p.set_defaults(fn=cmd_resolve)
    p = sub.add_parser("audit"); p.add_argument("build_dir")
    p.add_argument("--binary", help="a linked binary for the outside-the-kernels checks")
    p.add_argument("--rules", help="rules file (default: tools/kernel-audit.json)")
    p.set_defaults(fn=cmd_audit)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
