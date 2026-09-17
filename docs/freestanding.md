# Freestanding and RTOS builds

[← blake3pp](../README.md)

## Freestanding and RTOS builds

The library core runs on targets with no filesystem and no OS threads;
what drove this was a Zephyr SMP port running blake3pp on the RP2350
in both of its personalities (Cortex-M33 and Hazard3 RISC-V). Two
build-time switches make it fit:

- `-DBLAKE3PP_WITH_IO=OFF` drops the file-I/O layer entirely. It needs
  a filesystem, 64-bit seeks and OS-specific async I/O, none of which
  a microcontroller RTOS has; without it the library is `hasher`,
  `digest`, dispatch and the scheduler-taking parallel API. The
  umbrella header follows the option, and tests, tools and the file
  bench gate themselves on it (the in-memory bench and the non-I/O
  test suite still build).
- Toolchains without OS threads are detected, not fought: where
  `<thread>`/`<mutex>` are empty (the Zephyr SDK's libstdc++ is built
  without gthreads), the `BLAKE3PP_HAS_STD_THREAD` probe comes back
  negative and `get_parallel_scheduler()` simply does not exist.
  Nothing else is lost: the scheduler-taking `hash()` overloads are
  the primary API anyway, and a freestanding caller brings its own
  scheduler because only it knows what its execution agents should be
  (on Zephyr SMP, for instance, one per core).
- The multi-core entry points spend stack on two sides, the calling
  thread and the scheduler's agents, and three switches size it:
  `stack_budget`, `BLAKE3PP_MAX_SIMD_DEGREE` and
  `BLAKE3PP_SUBTREE_FOLD`. See the next section.

Everything else adapts by the existing probes: 32-bit targets are
supported, and the SIMD/execution polyfills select exactly as on
hosted platforms.

#### Stack requirements

Nothing in the compute paths allocates, so every byte a hash costs is
stack, on two threads at once: the one that calls `hash()` and each agent
the scheduler runs the parts on. They are provisioned separately.

**The calling thread** holds the part table, one 32-byte chaining value
per part, which is exactly what the `stack_budget` template argument
bounds: `budget.bytes`, 32 KiB by default. Nothing else about the input
adds to it.

**Each agent** reduces one part to a chaining value, and that is where
the rest lives. The default reduction recurses, holding one buffer per
level; the depth is how often the part halves before reaching twice the
running kernel's `simd_degree` in chunks:

```
part chunks = bit_ceil(max(input chunks / budget.parts(), 16))
depth       = log2(part chunks / (2 * simd_degree))
agent stack = entry frame + depth * per-level frame
```

The frames follow `BLAKE3PP_MAX_SIMD_DEGREE`, since the buffers are sized
for the widest kernel the build may contain, at `128 + 144 * degree`bytes
per level. Measured with g++ 16 at `-O2` on x86-64:

| `BLAKE3PP_MAX_SIMD_DEGREE` | entry frame | per level | opt-in fold, 12 levels | fold, 54 levels |
|---|---|---|---|---|
| 16 (default) | 5536 | 2432 | 3120 | 4464 |
| 8 | 2832 | 1280 | 1856 | 3200 |
| 1 | 480 | 272 | 720 | 2064 |

Two consequences follow when sizing a thread. A smaller budget
does not only shrink the table: fewer parts make each part larger, which
makes the recursion deeper, so it moves stack from the caller to the
agents. And `BLAKE3PP_SUBTREE_FOLD=<levels>` removes the depth term
altogether, at 32 bytes per level of its own, which is why its figures
above do not depend on the input.

A worked case, a Zephyr port: a scalar-only
build (`BLAKE3PP_MAX_SIMD_DEGREE=1`) hashing 1 MiB with
`stack_budget{1024}`, so 32 parts of 32 chunks and a depth of 4. The
caller needs 1 KiB for the table, each agent 480 + 4 * 272 = 1568 bytes,
or 720 bytes flat with `BLAKE3PP_SUBTREE_FOLD=12`.

These are one toolchain's frames; another compiler, ABI or optimization
level will differ. Measure yours the same way, by building the library
with `-fstack-usage` and reading the `.su` entries for
`compress_subtree_wide` and `compress_subtree_to_cv_recursive` (or
`compress_subtree_to_cv_folded` with the fold on), and leave the thread
its own headroom on top.
