# Freestanding and RTOS builds

[← blake3pp](../README.md)

The library core runs on targets with no filesystem and no OS threads;
the reference port is Zephyr SMP on the RP2350 in both of its
personalities, Cortex-M33 and Hazard3 RISC-V.

Three build-time switches make it fit:

- `-DBLAKE3PP_WITH_IO=OFF` drops the file-I/O layer entirely. It needs
  a filesystem, 64-bit seeks and OS-specific async I/O, none of which
  a microcontroller RTOS has; without it the library is `hasher`,
  `digest`, dispatch and the scheduler-taking parallel API. The
  umbrella header follows the option, and tests, tools and the file
  bench gate themselves on it (the in-memory bench and the non-I/O
  test suite still build).
- Toolchains without OS threads are detected: where
  `<thread>`/`<mutex>` are empty (the Zephyr SDK's libstdc++ is built
  without gthreads), the `BLAKE3PP_HAS_STD_THREAD` probe comes back
  negative and `get_parallel_scheduler()` does not exist.
  Nothing else is lost: the scheduler-taking `hash()` overloads are
  the primary API, and a freestanding caller brings its own
  scheduler because only it knows what its execution agents should be
  (on Zephyr SMP, for instance, one per core).
- The multi-core entry points spend stack on two sides, the calling
  thread and the scheduler's agents, and four switches size it:
  `stack_budget`, `BLAKE3PP_MAX_SIMD_DEGREE`, `BLAKE3PP_SUBTREE_GROUP`
  and `BLAKE3PP_SUBTREE_FOLD`. See the next section.

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

**Each agent** reduces one part to its chaining values, and that is where
the rest lives. The reduction stages a group of chunk batches, then
compresses their parents in one burst, so its working set is two staging
buffers and a bounded chaining-value stack. None of that depends on the
part's size: an agent's frame is a constant of the build, not of the
input.

Three build settings size it. `BLAKE3PP_MAX_SIMD_DEGREE` sets how wide
the staging buffers are, since they are sized for the widest kernel the
build may contain rather than the one that runs.
`BLAKE3PP_SUBTREE_GROUP` is how many chunk batches one burst stages, 8 by
default. `BLAKE3PP_SUBTREE_FOLD=<levels>` replaces the sweep with the
single-leaf fold, whose staging is one batch and whose stack is the levels
asked for. It applies to both forms of the reduction, the part form the
agents run and the whole-subtree form a sequential caller runs; neither
reaches the recursion, whose frame grows with the subtree's depth. Measured with g++ 16 at `-O2` on x86-64, bytes per agent:

| `BLAKE3PP_MAX_SIMD_DEGREE` | default (group 8) | group 1 | fold, 12 levels | fold, 54 levels |
|---|---|---|---|---|
| 16 (default) | 12800 | 2016 | 3152 | 4496 |
| 8 | 6528 | 1120 | 1872 | 3216 |
| 1 | 1056 | 1056 | 768 | 2112 |

A thread that hashes **sequentially** pays the same kind of frame, for the
whole-subtree form of the reduction rather than the part form: 14496 bytes
at degree 16, 8224 at 8, and 2752 on a scalar build, falling to 768 with
`BLAKE3PP_SUBTREE_FOLD=12`.

Two consequences follow when sizing a thread. `stack_budget` moves no
stack between the two sides: it bounds the caller's part table, and the
agents' frames are what the table above says whatever the parts' size. And
the cheapest agent on a small device is the fold, which is what it exists
for.

A worked case, a Zephyr port: a scalar-only build
(`BLAKE3PP_MAX_SIMD_DEGREE=1`) hashing 1 MiB with `stack_budget{1024}`, so
32 parts of 32 chunks. The caller needs 1 KiB for the table and each agent
1056 bytes, or 768 with `BLAKE3PP_SUBTREE_FOLD=12`.

These are one toolchain's frames; another compiler, ABI or optimization
level will differ. Measure yours the same way, by building the library
with `-fstack-usage` and reading the `.su` entries for
`compress_subtree_to_cvs_grouped` and `compress_subtree_to_cv_grouped`
(with the fold on, the first is the same function with one leaf per burst
and the second is `compress_subtree_to_cv_folded`), and leave the thread
its own headroom on top.
