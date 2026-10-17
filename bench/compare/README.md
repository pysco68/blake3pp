# The reference implementation as a baseline

`blake3pp_bench` already measures the upstream BLAKE3 C library's
hand-written assembly kernels through blake3pp's own pipeline, which holds
the kernel still and compares everything around it. This directory is the
other half of the comparison: the reference implementation in its own
natural configuration, the `blake3` crate with `rayon` driving the tree and
`update_mmap_rayon` reading files, so a row exists for the implementation
as its users get it.

It is a Cargo project because that is how the crate ships. Nothing in the
library depends on it, and nothing here is built unless asked for.

## Building it

With a Rust toolchain on the machine:

```bash
cmake --preset linux-gcc16-cxx26 -DBLAKE3PP_BENCH_COMPARE=ON
cmake --build build/linux-gcc16-cxx26 --target blake3pp_bench_compare
```

The binaries land in `<build>/bench/compare/`, one per compile-time cap
(see below), and the bench scripts that compare the two implementations
read them from there.

Or with cargo alone:

```bash
cargo build --release
./target/release/blake3-rayon-bench --size 512 --reps 5 --threads N
./target/release/blake3-rayon-bench --file BIG --reps 5
```

## One binary per instruction set

The crate picks its backend at runtime but is capped at compile time,
through negative features, so a sweep across instruction sets needs one
binary per cap rather than one binary with a flag:

| feature | what it caps |
|---------|--------------|
| (none) | the crate's own runtime choice, the fastest it has |
| `cap_avx2` | no AVX-512 |
| `cap_sse41` | no AVX-512, no AVX2 |
| `cap_sse2` | no AVX-512, no AVX2, no SSE4.1 |
| `cap_no_neon` | aarch64 without NEON |
| `portable` | the crate's pure-Rust path |

`cross-linux.sh x86_64|arm64|riscv64`, `cross-windows.sh x64|arm64` and,
on a Mac, `native.sh` build the whole set statically for a machine that
compiles nothing, plus a POSIX-sh runner, under `dist/`.

## What the two measurements answer

The in-memory mode (`--size`) isolates the execution model: no storage, no
page cache, nothing but scheduling. It also hashes the same bytes as 8 KiB
messages, one hasher each, which is the short-message path with no tree to
spread over cores and no long run to amortise a batch over;
`blake3pp_bench` has that row under both implementations (`blake3pp_8k`,
`reference_8k`), so the three are directly comparable. Its digest is
reported as `fold` rather than `digest` so that sweep scripts still read
one digest line per binary.

The file mode (`--file`) runs `update_mmap_rayon`, which the crate's
documentation recommends and which goes through the page cache by design.
That is measured as it ships rather than bent to match a direct-I/O
pipeline, since the difference is part of the answer.

## The protocol a number has to come from

A machine that does not thermally throttle, both tools pinned to the same
core count, the same size, five reps best-of, runs alternating A/B/B/A. For
the file comparison, leave each tool in its natural configuration; the file
mode drops the page cache before every timed repetition itself when run as
root (on Windows: elevated, emptying the working sets and purging the
standby list), and says so when it cannot. `--warm` skips the drop, as the
control row that shows what the drop is worth.
