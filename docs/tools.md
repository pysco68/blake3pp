# Command-line tools

[← blake3pp](../README.md)

Two utilities build alongside the library, in top-level builds only.
`blake3ppsum` hashes files the way `sha256sum` does. `blake3ppgen` produces
reproducible test data at the speed of your disk. Both are built on the
library this documents, so they are also worked examples of it.

The help below is what the tools print. Two parts of it depend on the
machine rather than on the documentation, and are shown as placeholders:
the variants a build carries, and the core count a thread default picks
up.

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
          --no-async          read synchronously (no io_uring/IOCP/GCD queue)
          --inline-submit     issue reads inline in the submitting thread, not on io_uring's
                              workers
          --no-ioring         Windows: read through the completion port instead of the I/O
                              ring
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
                              with --output: no async queue (synchronous writes)
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

`--no-direct`, `--no-async`, `--inline-submit` and, on Windows,
`--no-ioring` switch those off one at a time, which is how the pipeline
gets measured against itself.

## Tracing

Neither tool records a trace of its own. What they expose are the knobs a
trace settles: `--window`, `--qd`, `--threads`, `--no-direct` and
`--inline-submit`, the same names on both, and the same pipeline
underneath. `blake3ppgen -v` names the write backend that was engaged;
that is as much as either tool says about the pipeline it ran.

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

## The allocator

For the **tools** (blake3ppsum, blake3ppgen, benchmarks),
[mimalloc](https://github.com/microsoft/mimalloc/) is the default
allocator on every supported target (`BLAKE3PP_TOOL_MIMALLOC`, off only
for wasm and macOS).

The library itself stays allocator-neutral.
