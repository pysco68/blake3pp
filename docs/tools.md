# Command-line tools

[← blake3pp](../README.md)

## Command-line tools

Two utilities build alongside the library (top-level builds only):

**`blake3ppsum`** is a `sha1sum`-style checksum tool covering the full
spec:

- `--keyed FILE`: MAC mode; the key is 32 raw bytes or 64 hex chars,
  read from a file so it never appears on the command line
- `--derive-key CONTEXT`: domain-separated KDF mode
- `--length N`: extended (XOF) output
- `--check`: verify previously printed checksum lines
- `--arch`, `--threads` (default: all cores; 1 = sequential), and the
  I/O pipeline knobs
- `--version`: the providers and SIMD variants baked into the binary

```bash
blake3ppsum big.iso                          # multi-core, direct-I/O
blake3ppsum --keyed key.hex manifest/* > sums && blake3ppsum --keyed key.hex -c sums
blake3ppsum --derive-key "backup 2026 v1" --length 64 master.key
```

**`blake3ppgen`** is a deterministic, *seekable* byte-stream generator
built on extended output: the same seed (`--seed`, or the bytes of a
`--seed-file` streamed through the file pipeline) always yields the same
infinite stream, and `--seek` is O(1), so materializing a slice at
offset 10 GB costs the same as offset 0. Generation runs lanes-parallel
in the kernel and `--threads` fans segments across cores via the O(1) seek, 
so the sink is the bottleneck;
`--output` removes even that overhead, writing through io_uring +
O_DIRECT on Linux, IOCP + no-buffering on Windows or GCD + F_NOCACHE on
macOS with the stream generated straight into the write buffers,
bypassing the page cache entirely. The writer has the reader's pacing
knobs: `--window` sizes each write buffer (default 4 MiB per generator
thread, capped at 64 MiB), `--qd` sets how many are in flight
(default 4), and `--inline-submit`, `--no-direct` and
`--no-async` switch the io-wq hand-off, direct I/O and the async queue
off for A/B measurements:

```bash
blake3ppgen --seed run42 --length 1G > testdata.bin
blake3ppgen --seed run42 --seek 10G --length 1M > slice.bin   # instant
blake3ppgen --seed run42 --length 100G --output fixture.bin   # device-bound
blake3ppgen --seed run42 --length 32G --output f.bin --window 64 --qd 8
```

## The allocator

For the **tools** (blake3ppsum, blake3ppgen, benchmarks), [mimalloc](https://github.com/microsoft/mimalloc/) 
is the default allocator on every  supported target (`BLAKE3PP_TOOL_MIMALLOC`, off only for wasm and macos).

The library itself stays allocator-neutral.
