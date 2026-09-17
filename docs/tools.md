# Command-line tools

[← blake3pp](../README.md)

Two utilities build alongside the library, in top-level builds only.
`blake3ppsum` hashes files the way `sha1sum` does. `blake3ppgen` produces
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
          --inline-submit     issue reads inline in the submitting thread, not on io_uring's
                              workers
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
          --window UINT Needs: --output
                              with --output: write buffer size in MiB (default: 4 MiB per
                              generator thread, so every fill fans out fully)
          --qd UINT [4]  Needs: --output
                              with --output: write buffers in flight (the writer clamps it to
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
| Windows | IOCP + no buffering |
| macOS | GCD + `F_NOCACHE` |

`--no-direct`, `--no-async` and `--inline-submit` switch those off one at
a time, which is how the pipeline gets measured against itself.

## The allocator

For the **tools** (blake3ppsum, blake3ppgen, benchmarks),
[mimalloc](https://github.com/microsoft/mimalloc/) is the default
allocator on every supported target (`BLAKE3PP_TOOL_MIMALLOC`, off only
for wasm and macOS).

The library itself stays allocator-neutral.
