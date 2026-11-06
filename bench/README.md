# bench/

`blake3pp_bench` (throughput.cpp) times the kernels, the end-to-end
hasher and the parallel engine in memory, against upstream's kernels
where they build; `blake3pp_bench_file` (file_throughput.cpp) times the
file pipeline against the device's raw read rate. Both build with
`BLAKE3PP_BUILD_BENCH` and are documented by their `--help`.

`blake3pp_dispatch_cost_<variant>` and `blake3pp_detect_cost` each answer
one question and are off by default (`-DBLAKE3PP_BENCH_DISPATCH_COST=ON`;
not built for MSVC-style compilers or Emscripten).

`pipe/` holds the instruments for measuring a stream through a pipe and
the scripts that run them; see [Pipe instruments](#pipe-instruments)
below. They are standalone programs that build on every platform,
Windows included.

`compare/` holds the reference implementation's own bench, the blake3
crate over rayon, in its own Cargo project: `-DBLAKE3PP_BENCH_COMPARE=ON`
builds one binary per compile-time SIMD cap into `<build>/bench/compare/`
when cargo is on PATH. See [compare/README.md](compare/README.md) for the
caps, the two measurements and the protocol. The upstream C library's
kernels are a different comparison and need no Rust: they build into
`blake3pp_bench` itself (`BLAKE3PP_BENCH_UPSTREAM`, on by default).

## --null-source: a ceiling, not a working set

`blake3pp_bench_file --null-source N` runs the pipeline over a file that
does not exist. It reports N GiB, but the driver owns only
`windows x window_bytes` of buffer and every window reuses its slot's
slice; a read completes with whatever is already there. What that
measures is the engine, the fan-out and the fold with the device
removed, which is the ceiling a real device is measured against.

Two things follow, and neither is a defect:

- **The bytes are cache-resident by construction.** A handful of windows
  is all there is, so wherever the L3 holds them the per-core rate is
  higher than any real device could reproduce -- with DMA every window
  lands in DRAM whatever the count. Read a null-source rate as an upper
  bound on the engine, never as a throughput anyone can reach.
- **It is not comparable to `blake3pp_bench`'s `hash()` rows.** Those
  hash a real span, so they pay for a working set the caches do not
  hold, and they exercise the same pipeline over a source that has
  distinct bytes at every offset. The two numbers answer different
  questions and must not be put in one column.

## --null-sink: the same for the write pipeline

`blake3pp_bench_file --null-sink N` is the mirror image: the write
pipeline generates N GiB into a sink that completes each write from
`poll()` and stores nothing. What is left is the generator, the sender
chain and the driver loop, with the device removed.

The caveats above apply unchanged, and one more is specific to this
side. **The number scales with the queue depth, not with the core
count.** A window's fill is sequential -- the parallelism is the windows
in flight, not a fan-out inside one -- so the useful reading is a sweep
of `--qd` at a fixed `--window`, and the window size barely moves it.
On a Ryzen 7 PRO 6860Z (16 threads), 2 GiB, best of 3:

| geometry | GiB/s | | geometry | GiB/s |
|---|---|---|---|---|
| 64 MiB x qd 2 | 8.56 | | 16 MiB x qd 8 | 16.2 |
| 64 MiB x qd 4 | 14.6 | | 8 MiB x qd 16 | 17.1 |
| 64 MiB x qd 8 | 16.8 | | 4 MiB x qd 16 | 18.5 |
| 64 MiB x qd 16 | 16.9 | | | |

## A modelled device under the null source and sink

`--null-rate R` gives `--null-source` and `--null-sink` a device of R
GiB/s in place of immediate completion: one FIFO server with a fixed
service time per window and a propagation delay of `--null-knee` minus
one service times, so throughput rises linearly with the queue depth up
to the knee and is flat above it. The run prints the service time and
latency it derived. It shows how the pipeline and `--diag`'s counters
behave against a device whose shape is known.

## The flags

`--help` is authoritative; this is the map.

`blake3pp_bench_file`, over a real file: `--reps`, `--cooldown`,
`--window` (MiB), `--qd`, `--no-direct`, `--no-async` (the synchronous
rung, against the platform's async engine), `--inline-submit`,
`--threads`, `--no-ioring` and `--registered-buffers` (Windows: the
completion port instead of the I/O ring, and the ring's buffer
registration), `--seq-only`, `--trace PATH` with `--trace-agents`,
`--io-sweep` (the window x qd pacing matrix), and `--make <MiB>` to
create a file and use it. Without a device: `--null-source <GiB>` and
`--null-sink <GiB>`, with `--null-rate` and `--null-knee` for a modelled
device.

`blake3pp_bench`, in memory: `--size` (MiB), `--reps`, `--cooldown`,
`--pin`, `--threads`, and a list of SIMD variants to
measure.

## file_throughput: where a window's time goes (`--trace`)

`blake3pp_bench_file FILE --trace PATH` hands the pipeline a
`blake3pp::trace_buffer` (`<blake3pp/trace.hpp>`) sized for the file:
one `window_record` per I/O window, and with `--trace-agents` one
`agent_record` per pool thread per window. Every rep starts from an
empty buffer; the last rep of each row (`seq`, `parallel`) is what the
summary below the row describes and what lands in PATH as Chrome trace
JSON (with both rows, `PATH` becomes `<stem>.seq.<ext>` and
`<stem>.parallel.<ext>`). Open it in Perfetto or `chrome://tracing`.

A window record holds five timestamps, nanoseconds on the steady clock
from the buffer's construction: before the reader was asked for the
window (`t_wait_begin`), the reader handing it out (`t_ready`), the
compute done (`t_joined`), the chaining values in the hasher
(`t_absorbed`, equal to `t_joined` for a window hashed inline) and the
buffer back with the reader (`t_released`). The JSON puts the four
intervals on `tid` 1 as `wait`, `hash`, `absorb` and `release`, one
`"X"` event each with the window index in `args`. The summary prints
their shares of the row's wall time (last release minus first wait),
and for the parallel row the pool's busy fraction (the agents' summed
compress time over threads × wall) and how many agents took part per
window.

An agent record is one bulk invocation that compressed at least one
part (a part is 16 or more chunks; nothing below a part is timed): when
it started, its busy time, the parts it took, the shortest and longest
part, and the CPU it ran on. The JSON gives each CPU its own track
(`tid` 100 + cpu; 99 when unknown) with one `compress` event per
record, so the pipeline row sits above the cores. The summary aggregates
per CPU into an effective rate (bytes compressed over busy time), then
prints the run's min, median and max of that rate and the worst
`max_part / min_part` ratio seen on one agent. A slow core is a low
rate; preemption is a large ratio on an otherwise normal core; cores
that arrive after the counter is drained are missing altogether. The
per-window overhead of `--trace` alone is a few clock reads on the
driving thread; `--trace-agents` adds one clock read per part on the
agents.

On Linux the summary also reports `io-wq cpu`, the CPU time of the
kernel's io_uring worker threads (`iou-wrk-*` under `/proc/self/task`)
over the rep's wall time, in cores. Workers come and go with the load,
so the figure only credits workers alive at both ends of the rep.

### Pairing with perf

The records and `perf` share `CLOCK_MONOTONIC`; the JSON's top-level
`epoch_ns` is the records' zero on that clock:

    perf record -k CLOCK_MONOTONIC -e cycles --call-graph lbr -- \
        blake3pp_bench_file FILE --trace t.json --trace-agents
    perf script -F time,cpu,ip,sym

A sample at time `T` seconds belongs to the window whose interval
covers `T * 1e9 - epoch_ns`, and to the agent record with the same
`cpu` whose `[t_begin, t_begin + busy_ns)` covers it. No probes or
markers are involved; the join is by time and CPU alone.

## dispatch_cost: what the fat binary pays at the call boundary

Every hashing call in the library goes through a pointer to the running
variant's `kernel_ops` table (`hasher::update` is one memory-indirect
call), and the kernel bodies live in translation units of their own, so
nothing can be inlined across that boundary. This bench isolates what
that costs: the kernel TU is compiled into the bench itself, once per
variant (`scalar`, `sse42`, `avx2`, `avx512` on x86-64; `scalar` and
`neon` on aarch64; `scalar` and `rvv256` on riscv64; each only where the
library registered it, with the flags of `blake3pp_add_kernel()`), and
the same body is called three ways on one pinned core:

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

### The detection ladder

`blake3pp_detect_cost`, built with the same option, times a fresh
process's first library call (`best_available()`, which builds the
availability table from auxv, hwprobe or CPUID once per compiled
variant), the cached second call, a `hasher{}` construction, the
opt-in `run_trap_probes()` rung, and one `is_available()` per compiled
variant. Only a new process pays the first call, so run it several
times. Under a hypervisor the microseconds are `cpuid` trapping to the
host; on bare metal the whole ladder is about a microsecond.

### Other compilers and ISAs

The call site is the only thing that differs, and the disassembly shows it
without running anything: under GCC the dispatched call is a register
load plus an indirect call on x86-64 (`mov; call *%r10`), AArch64
(`ldr; blr`) and RISC-V (`ld; jalr`), one instruction more than the
direct call; under clang (the zig presets) it is a single
memory-indirect call (`call *0x28(%r12)`), the same count as a direct
call. The zig x86-64 build is static and runs on any x86-64 host, so
it also measures natively. qemu-user is no help here: the toolchain
images' qemu has no TCG plugins (no guest instruction count), and TCG
timings say nothing about a CPU's indirect branches.

### Results

Instructions, Zen 3 (Ryzen 7 PRO 6860Z), gcc 16: the dispatch is one
instruction, the load of the function pointer; the indirect call
replaces a direct call one for one and is predicted (the branch-miss
rate through the table equals the rate by name). At the leaf batch that
is 0.002–0.003 % of the instructions on every variant, flat across
widths, because a wider kernel is dispatched half as often per byte and
does half the instructions per chunk. Per 64-byte block it is 0.1 % of
the instructions; under clang, zero extra instructions at the leaf batch
and two per block.

Time, on idle cloud servers (Neoverse V2, Neoverse N1 and Zen 5, medians
of 9 interleaved reps): at one call per 64-byte block, dispatched and
by-name calls are within 0.1 ns of each other on every core and
variant. At the leaf batch they differ by 0.1–2 %, with the sign changing
between shapes and machines, which is the run-to-run spread.

## Pipe instruments

`bench/pipe/` measures what `blake3ppsum -` does with a stream: small
programs that each do one job, and two scripts that run them. They link
no blake3pp library, only CLI11 (and stdexec for `thief`), so they build
wherever the compiler does. On Windows they build as the `pipe_*`
targets even though the bench executables above do not link there. They
are not built for Emscripten.

| program | measures | platforms |
|---|---|---|
| `pump` | Producer. Writes a generated pattern (`--gen GiB`) or a file (`--file`) in fixed `--chunk KiB` writes, optionally paced (`--gap-us`). Fault modes: `--close-after N`, `--die-after N` (SIGKILL / TerminateProcess), `--stall-ms T --after N`, and `--wait-for PATH --after N` to hold the stream until another process signals. | all |
| `drain` | Consumer that discards the bytes. One blocking reader; `--threads N` for readers that always have a read posted (the never-sleeping drain); `--watch PROBE` to probe before each read by the stream reader's rule. `--close-after N` closes its stdin under the writer, and `--mark PATH` signals that it has. Widens a Linux pipe as `update_stream` does (`--widen`). Reports bytes, reads, the pipe buffer and how the stream ended. | all; `--watch` probes are per platform |
| `rux` | Runs one command and reports its exit code, wall clock, user and system CPU, and voluntary and involuntary context switches; exits with the command's code. `--sigpipe`, `--signal-storm`, `--nonblock-stdin`, `--close-stdin` set up a fault. | all; context switches read `na` on Windows, and `--sigpipe` and `--signal-storm` are POSIX |
| `pipeprobe` | Re-checks the pipe facts the stream reader's comments cite: `depth` (what a pipe holds with nothing reading), `cost` (one readiness probe, clock read and busy-wait hint), `price` (whether a read costs by the range it asks for). | all |
| `libpipe_readshim` | Preloaded read() and poll() counter, a shared library (on Linux and FreeBSD it also takes the fortified `__read_chk` and `__poll_chk`, and `ppoll`). Can inject EIO after N bytes (`READSHIM_FAIL_AFTER`) or make a signal interrupt reads (`READSHIM_EINTR`). | Linux, FreeBSD, macOS; not Windows |
| `thief` | Context switches an idle stdexec pool costs one busy task, per millisecond. | POSIX with stdexec |

Where a tool cannot be portable, the source says why:

- Windows has no per-process context-switch counter that outlives the
  process, so `rux` reports `na` there.
- Windows has no preload mechanism short of a code-rewriting hook, so
  `readshim` is not built there.
- `readshim` cannot see reads that the C library's stdio makes through
  an internal symbol (glibc and Darwin both do this), nor a statically
  linked program's reads.
- Windows has no signals, so `rux --signal-storm` and `--sigpipe` are
  POSIX only.
- `thief` reads its context switches through `getrusage`, which Windows
  does not have, and it measures stdexec's pool, so it is built on POSIX
  with the stdexec provider only.

### Running a measurement

`runpipe.sh` (POSIX `sh`: Linux, macOS, FreeBSD) and `runpipe.ps1`
(Windows) run one producer into several consumers:

    sh bench/pipe/runpipe.sh -t <dir with the tools> -r 6 \
       -p 'pump --gen 2 --chunk 4' base='blake3ppsum -' t2='blake3ppsum --threads 2 -'

    powershell -ExecutionPolicy Bypass -File bench\pipe\runpipe.ps1 -ToolDir <dir> `
       -Producer 'pump.exe --gen 2 --chunk 4' "base=blake3ppsum.exe -"

Each side of every pipeline runs under `rux`, so both exit codes and
both processes' CPU are recorded whatever the shell does with a
pipeline's status. A run prints the producer's exact command, the
reference digest, the drain ceiling and the stream's length. It then
prints one row per round in run order, then per-arm medians. It stops
with a non-zero exit at the first round that:

- has a side that exited non-zero or produced no report;
- printed a digest other than the reference, which is taken once before
  the rotation;
- was more than 1.5 times faster than the drain ceiling: the best of four
  drain configurations over the same producer, each run twice. A round
  faster than the ceiling but within that margin is printed with a
  warning.

The arm order reverses every round. The Windows script runs each
pipeline through `cmd.exe`, whose anonymous pipe is the one a Windows
user's `x | blake3ppsum -` meets. A PowerShell pipeline between native
programs passes the bytes through PowerShell instead, and WSL's or Git
Bash's pipes are different pipes. `runpipe.sh` passes `shellcheck`
0.11.0 at `--severity=style` with the default checks; the
`koalaman/shellcheck-alpine:v0.11.0` image runs it from stdin:

    docker run --rm -i koalaman/shellcheck-alpine:v0.11.0 \
       shellcheck --shell=sh --severity=style - < bench/pipe/runpipe.sh

ctest runs the platform's script once (`pipe_runpipe_smoke`):
`runpipe.ps1` on Windows where PowerShell is found, `runpipe.sh`
elsewhere. The fault tests in
`cli/faults.cmake` (`cli_fault_*`) use the same tools to hold
`blake3ppsum` and `blake3ppgen` to their behaviour in a broken pipeline.

### Rules for a stream measurement

1. **Name the producer as its exact command.** Two producers of the same
   bytes give the same consumer different figures. On one machine at one
   commit, `cat FILE` from the page cache fed `blake3ppsum -` at 7026
   MiB/s and `head -c 2G /dev/zero | cat` at 1952. At 4 KiB writes, `dd`
   reading a file put it at 0.84x `b3sum` and `dd` reading `/dev/zero`
   at 2.1x, so the producer can turn a loss into a win.
2. **Report the producer's CPU and the two-process total**, never the
   consumer's alone. A consumer change can move cost into the producer:
   a reader that sleeps makes every write a wake-up.
3. **Counterbalance**: run the arms on one machine, alternating, with
   the order reversed every round, and report every round in run order
   so drift is visible.
4. **Check every figure against a drain ceiling.** A harness whose
   command failed at once has reported 2 GiB in 0.013 s, 155 000 MiB/s.
   The drain must read the same pipe as the consumer, so it widens a
   Linux pipe to 1 MiB, as `update_stream` does; a drain on the default
   64 KiB pipe is no ceiling for a `blake3ppsum` that widened it. On
   Windows the drain's ask must match the producer's write, because a
   read is priced by the range it asks for.

Result files do not go in the repository.
