# Command-line tools

[← blake3pp](../README.md)

Two utilities build alongside the library, in top-level builds only.
`blake3ppsum` hashes files the way `sha256sum` does. `blake3ppgen` produces
reproducible test data at the speed of your disk. Both are built on the
library this documents, so they are also worked examples of it.

The help below is what the tools print. Three parts of it depend on the
build or the machine rather than on the documentation, and are shown as
placeholders: the variants a build carries, the core count a thread
default picks up, and the async I/O engines a build carries. The last is
not written down anywhere: `--no-async` names the engines from
`blake3pp::compiled_io_engines()`, so it says `io_uring` on Linux,
`ioring/iocp` on Windows, `gcd` (`gcd-write` on the write side) on
macOS, `aio` on FreeBSD, and `async` where the platform has no async
engine to turn off. See [the API reference](api.md) for the
introspection calls behind it.

## blake3ppsum

Plain, keyed and derive_key hashing, extended output, and `--check` to
verify checksum lines it printed earlier.

```
Print or check BLAKE3 checksums.
With no FILE, or FILE of '-', read standard input.


blake3ppsum [OPTIONS] [files...]


POSITIONALS:
  files TEXT ...              files to hash (or checksum lists)

OPTIONS:
  -h,     --help              Print this help message and exit
          --version           Display program version information and exit
  -c,     --check             read checksum lines from FILEs and verify them
          --keyed TEXT Excludes: --derive-key
                              keyed (MAC) mode; FILE holds the 32-byte key as raw bytes or 64
                              hex chars ('-' reads it from stdin)
          --derive-key TEXT Excludes: --keyed
                              key-derivation mode with this context string
          --length UINT:UINT in [1 - 1048576] [32]
                              output length in bytes (extended output)
          --arch ENUM:value in {auto, scalar, every variant this build carries} [auto]
                              pin a SIMD variant
          --threads UINT:THREADS [<cores>]
                              compute threads (1 = sequential; default: all)
          --window UINT:MiB [8]
                              I/O window size in MiB
          --qd UINT [4]       I/O queue depth (the reader clamps it to its range)
          --no-direct         keep the OS page cache (no O_DIRECT)
          --no-async          read synchronously (no <this build's async engines> queue)
          --inline-submit     issue reads inline in the submitting thread, not on io_uring's
                              workers
          --no-ioring         Windows: read through the completion port instead of the I/O
                              ring
          --trace TEXT        record per-window timing and write it as Chrome trace JSON to
                              PATH (open in ui.perfetto.dev)
          --trace-agents Needs: --trace
                              with --trace: also record each pool agent's part of every window
          --diag              report on stderr which side limited the run (the hash, the
                              device, or the window arena between them); one FILE or '-', on
                              the pipelined path
```

```bash
blake3ppsum big.iso                          # multi-core, direct-I/O
blake3ppsum --keyed key.hex manifest/* > sums && blake3ppsum --keyed key.hex -c sums
blake3ppsum --derive-key "backup 2026 v1" --length 64 master.key
```

## blake3ppgen

A deterministic, *seekable* byte-stream generator built on extended
output. One seed always yields the same stream, and `--seek` is O(1), so
materializing a slice at offset 10 GB costs what a slice at offset 0
costs.

```
Deterministic seekable byte stream from BLAKE3 extended output.
The same seed always yields the same stream; --seek is O(1).


blake3ppgen [OPTIONS]


OPTIONS:
  -h,     --help              Print this help message and exit
          --seed TEXT Excludes: --seed-file
                              seed string
          --seed-file TEXT Excludes: --seed
                              read seed bytes from FILE
          --derive-key TEXT   domain-separate the stream with this context string
          --length TEXT [inf]
                              bytes to emit: N, NK/NM/NG/NT, or 'inf'
          --seek TEXT [0]     starting offset in the stream
          --hex Excludes: --output
                              emit lowercase hex instead of raw bytes
          --output TEXT Excludes: --hex
                              write to FILE via direct async I/O (io_uring or IOCP where
                              available) instead of stdout
          --no-direct Needs: --output
                              with --output: no direct I/O (write through the page cache)
          --no-async Needs: --output
                              with --output: write synchronously (no <this build's async
                              engines> queue)
          --inline-submit Needs: --output
                              with --output: issue writes inline in the submitting thread, not
                              on io_uring's workers
          --no-ioring Needs: --output
                              with --output, Windows: write through the completion port
                              instead of the I/O ring
          --waive-zero-fill Needs: --output
                              with --output, Windows: set the valid data length up front
                              instead of writing a sparse file (needs the volume-maintenance
                              privilege; exposes stale disk blocks until written)
          --trace TEXT Needs: --output
                              with --output and --length, on the pipelined path: record
                              per-window timing and write it as Chrome trace JSON to PATH (open
                              in ui.perfetto.dev)
          --trace-agents Needs: --trace
                              with --trace: also record which CPU filled each window, and for
                              how long
          --diag Needs: --output
                              report on stderr which side limited the run (the generator or the
                              device); needs --output and --length on the pipelined path
          --window UINT Needs: --output
                              with --output: write buffer size in MiB (default: 4 MiB per
                              generator thread, so every fill fans out fully)
          --qd UINT Needs: --output
                              with --output: write buffers in flight (default: one per
                              generator thread, or 2 with --threads 1; the writer clamps it to
                              its range)
  -v,     --verbose           report the engaged write backend on stderr
          --threads UINT:THREADS [<cores>]
                              generator threads (1 = sequential; default: all)
```

```bash
blake3ppgen --seed run42 --length 1G > testdata.bin
blake3ppgen --seed run42 --seek 10G --length 1M > slice.bin   # instant
blake3ppgen --seed run42 --length 100G --output fixture.bin   # device-bound
blake3ppgen --seed run42 --length 32G --output f.bin --window 64 --qd 8
```

### Where the time goes

Generation runs lanes-parallel in the kernel, and `--threads` fans
segments across cores using that same O(1) seek. Which leaves the sink as
the bottleneck.

`--output` removes even that. The stream is generated straight into the
write buffers and leaves through the platform's own direct path,
bypassing the page cache entirely:

| OS | writer |
|----|--------|
| Linux | io_uring + `O_DIRECT` |
| Windows | IoRing (Windows 11) or IOCP + no buffering |
| macOS | GCD + `F_NOCACHE` |
| FreeBSD | POSIX AIO on a kqueue + `O_DIRECT` |

`--no-direct`, `--no-async`, `--inline-submit` and, on Windows,
`--no-ioring` switch those off one at a time, which is how the pipeline
gets measured against itself.

## Tracing

Both tools record their run as Chrome trace JSON with `--trace PATH` (and
`--trace-agents`): `blake3ppsum` its batch path, a directory of files as
their windows across the context's window objects; `blake3ppgen` its
pipelined write path, each window's queue, fill, write and release. What
they also expose are the knobs a trace settles: `--window`, `--qd`, `--threads`, `--no-direct` and
`--inline-submit`, the same names on both, and the same pipeline
underneath. `blake3ppgen -v` names the write backend that was engaged;
that is as much as either tool says about the pipeline it ran.

`blake3pp_bench_file` takes `--no-async` as well, which is how the
platform's async engine gets measured against its synchronous rung on
one device: the same windows, the same queue depth, served by `pread`
(`ReadFile` on Windows) instead. Its help text names the engines the
same derived way the other two do.

To see where a run's time actually goes, point `blake3pp_bench_file` at
the same file with the same flags and add `--trace`:

```bash
blake3ppsum --window 32 --qd 16 big.iso
blake3pp_bench_file big.iso --window 32 --qd 16 --trace t.json --trace-agents
```

It prints a per-window summary and writes the run as a Chrome trace. The
summary gives the wait/hash/absorb/release shares, the driver's own busy
fraction, and with `--trace-agents` an effective rate per CPU.
[Performance tracing](tracing.md) explains the records, the summary and
the trace file, and [Where a window's time goes](https://github.com/pysco68/blake3pp/blob/main/bench/README.md)
has the bench's remaining rows.

## Diagnosing a run

`--diag` reports which side of the pipeline was the constraint on the run
that just happened, from counters the run already kept. It costs no
allocation and no per-window storage: every figure comes out of
`driver_stats` (see [Performance tracing](tracing.md#driver_stats)).

```
$ blake3ppsum --diag big.iso
diagnosis: device-limited
  pool busy        1.9% of 16 threads
  arena          6 of 16 windows in flight, 8 MiB each (128 MiB), wider than the queue depth of 4
  device         16.679 ms per window over 256 reads (min 9.845 ms, 4.0 of 4 queued)
  the pool had a window whenever it wanted one and still did not saturate, and the device's queue was full: it is the wall.
```

Three figures and a reading. **Pool busy** is the compute pool's share of
its own capacity. **Arena** is the window objects the plan allocated and
whether they outnumber the queue depth — the windows beyond the reads are
what the pool draws on while the device serves the next ones. **Device** is
the mean time a transfer spent at the device, the shortest such time, and
how full its queue was; the minimum is what makes the mean readable, since
a queue's latency is proportional to its depth by construction.

| reading | what it means |
|---|---|
| `hash-limited` | the pool was saturated; no I/O geometry moves it |
| `device-limited` | the pool had a window whenever it wanted one, did not saturate, and the device's queue was full |
| `arena-limited` | neither end was busy and the run held every window it had, with the arena no wider than the queue |
| `generator-limited` | writing: the windows spent more of the run being filled than being written |

A figure that lands near its ceiling without reaching it gets no label: the
run names the two readings it cannot separate and prints the figures.

```
$ blake3ppsum --diag --no-direct --threads 2 big.iso
diagnosis: hash-limited
  pool busy       95.6% of 2 threads
  arena          16 of 16 windows in flight, 8 MiB each (128 MiB), wider than the queue depth of 4
  device         1.531 ms per window over 256 reads (min 559.2 us, 2.2 of 4 queued)
  the pool was saturated: the hash is the constraint and I/O geometry will not move it.

$ blake3ppsum --diag --no-direct --window 8 --qd 16 big.iso
diagnosis: arena-limited
  pool busy       34.5% of 16 threads
  arena          16 of 16 windows in flight, 8 MiB each (128 MiB), no wider than the queue depth of 16
  device         3.977 ms per window over 256 reads (min 864.0 us, 11.2 of 16 queued)
  the pool idled waiting for a window while the device was not queued.
```

On the write side the arena and the depth are the same number — one window
is one fill task, so the windows in flight are the generator's parallelism
and the device's queue at once — so there are two readings rather than
three, and the report says which direction it is describing:

```
$ blake3ppgen --seed s --length 2G --output out.bin --diag
diagnosis: device-limited (write side)
  generator        2.8% of the 16 windows in flight (2.8% of 16 threads)
  windows        16 in flight, 4 MiB each (64 MiB); the arena and the depth are one number here
  device         15.083 ms per window over 512 writes (min 1.301 ms), holding  93.9% of the windows
  the device held a full queue for the run: it is the wall.
```

`--diag` describes one run: `blake3ppsum` takes exactly one FILE, not `-`
and not `--check`, and `blake3ppgen` needs `--output` and `--length` on the
pipelined path. A run too short to turn its queue over four times reports
that instead of a reading. The captures above are from a 16-thread Zen 5
machine under WSL2, on its virtual disk and through the page cache where
`--no-direct` appears.

## The allocator

For the **tools** (blake3ppsum, blake3ppgen, benchmarks),
[mimalloc](https://github.com/microsoft/mimalloc/) is the default
allocator on every supported target (`BLAKE3PP_TOOL_MIMALLOC`, off only
for wasm and macOS).

The library itself stays allocator-neutral.
