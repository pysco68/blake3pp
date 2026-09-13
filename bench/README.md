# bench/

Three programs. `blake3pp_bench` (throughput.cpp) times the kernels, the
end-to-end hasher and the parallel engine in memory, against upstream's
kernels where they build; `blake3pp_bench_file` (file_throughput.cpp)
times the file pipeline against the device's raw read rate. Both build
with `BLAKE3PP_BUILD_BENCH` and are documented by their `--help`.

The third, `blake3pp_dispatch_cost_<variant>`, answers one question and
is off by default (`-DBLAKE3PP_BENCH_DISPATCH_COST=ON`).

## dispatch_cost: what the fat binary pays at the call boundary

Every hashing call in the library goes through a pointer to the running
variant's `kernel_ops` table (`hasher::update` is one memory-indirect
call), and the kernel bodies live in translation units of their own, so
nothing can be inlined across that boundary. This bench isolates what
that costs: the kernel TU is compiled into the bench itself, once per
variant (`scalar`, `sse42`, `avx2`, `avx512` on x86-64; `neon` on
aarch64, with the flags of `blake3pp_add_kernel()`), and the same body
is called three ways on one pinned core:

| variant | shape |
|---|---|
| `dispatched` | a pointer to the table, laundered past the optimizer, and an indirect call through it: what `hasher` does |
| `direct` | a `noinline` wrapper: a call boundary without indirection (its own argument shuffle and tail jump cost 3–4 instructions per call, so it is a check, not the baseline) |
| `inlined` | the entry called by name in the same TU: the baseline, a direct call to the same body |

Three rows: one `compress_in_place` per 64-byte block (the path for
messages shorter than a chunk, the worst case), `hash_many` over
2 × `simd_degree` chunks per call (the library's leaf batch,
`src/core/subtree.hpp`), and 64 chunks per call. Per message the
program prints wall time and, on Linux, cycles, instructions, branches
and branch-misses read through `perf_event_open` around each loop
(user mode; the WSL2 kernel exposes the PMU too). Variants run
round-robin per rep and each figure is the median over the reps, so a
drift in clock lands on all three alike.

    cmake --preset linux-gcc16-cxx26 -B build/x -DBLAKE3PP_BENCH_DISPATCH_COST=ON
    cmake --build build/x --target blake3pp_dispatch_cost_avx2
    build/x/bench/blake3pp_dispatch_cost_avx2            # 64 MiB per rep, 9 reps
    build/x/bench/blake3pp_dispatch_cost_avx2 64 9       # the same, explicit

### Exact instruction counts with Intel SDE

Instruction counts are deterministic, so the cleanest measurement is a
dynamic instruction count per call shape, and it also covers a variant
the machine cannot run (avx512 on a Zen 3 through `-skx`). The last two
arguments restrict a run to one call shape (`0` dispatched, `1` direct,
`2` inlined) and one row (`0` blocks, `1` leaf batch, `2` 64-chunk
batch); the difference between two processes' totals is the mechanism:

    sde64 -skx -mix -omix d.txt -- build/x/bench/blake3pp_dispatch_cost_avx512 64 1 0 1
    sde64 -skx -mix -omix i.txt -- build/x/bench/blake3pp_dispatch_cost_avx512 64 1 2 1
    grep '^\*total' d.txt i.txt        # (dispatched - inlined) / calls = instructions per dispatch

A 2-rep run minus a 1-rep run of one shape gives the kernel's own
instructions per chunk. Use 64 MiB for the per-call differences (tens
of thousands of calls, so the few hundred instructions of differing
`printf` between processes vanish) and 8 MiB for the rest.

### Two compiler flags, and why

With the kernel body visible in the bench's TU, GCC does two things the
library's separate kernel TUs make impossible, and both turn the
`dispatched` loop into something else. It guesses the indirect call's
target and emits a compare and a branch to a direct call ahead of it
(`-fdevirtualize`; the pass covers plain function pointers with a
single address-taken candidate, not only virtual calls), and it
specialises a clone of the entry for the direct calls' constant
arguments (IPA-CP), so the three shapes stop running one body. The
bench compiles with `-fno-devirtualize -fno-ipa-cp-clone` under GCC so
that it measures what the library pays; the cloning flag moves the
body's own instruction count by a few tenths of a percent, which is the
one difference to the library's kernel objects. Without the flags the
bench reports two to four instructions per dispatch and means the
compare, the branch and a `lea`, none of which the library executes.

### Other compilers and ISAs

The call site is the whole question, and the disassembly answers it
without running anything: under GCC the dispatched call is a register
load plus an indirect call on x86-64 (`mov; call *%r10`), AArch64
(`ldr; blr`) and RISC-V (`ld; jalr`), one instruction more than the
direct call; under clang (the zig presets) it is a single
memory-indirect call (`call *0x28(%r12)`), the same count as a direct
call. The zig x86-64 build is static and runs on any x86-64 host, so
it also measures natively. qemu-user is no help here: the toolchain
images' qemu has no TCG plugins (no guest instruction count), and TCG
timings say nothing about a CPU's indirect branches.

### Result on the machine it was written on

Zen 3 (Ryzen 7 PRO 6860Z), gcc 16: the dispatch is one instruction,
the load of the function pointer; the indirect call replaces a direct
call one for one and is predicted (the branch-miss rate through the
table equals the rate by name). At the leaf batch that is 0.002–0.003 %
of the instructions on every variant, flat across widths, because a
wider kernel is dispatched half as often per byte and does half the
instructions per chunk; in cycles the difference sits inside the 2 %
run-to-run noise. Per 64-byte block it is 0.1 % of the instructions and
3–18 cycles of about 300 under GCC; under clang, zero extra
instructions at the leaf batch, two per block, and no cycle
difference at all.
