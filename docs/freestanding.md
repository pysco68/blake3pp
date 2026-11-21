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
asked for. It applies to both forms of the reduction, and neither frame
grows with the subtree's depth.

The reduction has two forms. The part form,
`compress_subtree_to_cvs_grouped`, reduces a part to the chaining values
the engine folds afterwards; the whole-subtree form,
`compress_subtree_to_cv_grouped`, reduces a subtree to one. Their frames,
measured with g++ 16.0.1 at `-O2` on x86-64 Linux (`-fstack-usage` on
`src/blake3pp.cpp`), in bytes:

| `BLAKE3PP_MAX_SIMD_DEGREE` | form | default (group 8) | group 1 | fold, 12 levels |
|---|---|---|---|---|
| 16 (default) | part | 12784 | 2032 | 2032 |
| | whole subtree | 14480 | 3728 | 2384 |
| 8 | part | 6512 | 1136 | 1136 |
| | whole subtree | 8208 | 2832 | 1488 |
| 1 | part | 1024 | 352 | 352 |
| | whole subtree | 2736 | 2064 | 720 |

The fold at 54 levels is the group 1 build, the same functions; each level
below 54 takes 32 bytes off the whole-subtree frame and nothing off the
part form, which has no stack. On the scalar builds with group 1 or the
fold, g++ inlines the part form into its caller `compress_subtree_cvs`,
and the cell is that caller's frame.

What a thread pays:

- An agent reducing a part of a full window runs the part form, and under
  it the whole-subtree form whenever one of the part's output chaining
  values covers a whole group: a part of at least
  2 · group · `simd_degree`² chunks for the running kernel's `simd_degree`,
  which is 4096 chunks at 16 lanes, 1024 at 8, and every part on a scalar
  kernel (16 chunks at the default group, 2 at group 1 or the fold). The
  agent that completes a group of parts then folds their chaining values,
  in `fold_sibling_cvs_wide`, after its own part: 3488 bytes at degree 16,
  1824 at 8 and 368 at 1.
- An agent reducing a part of an end window, or of a message that fits in
  one window, runs the whole-subtree form alone.
- A thread that hashes **sequentially** runs the whole-subtree form under
  `compress_chunks`, which stages one chunk batch's chaining values: 1456
  bytes at degree 16, 816 at 8, and 320 at 1, where it is inlined into
  `hasher::update`.

The kernel's `hash_many` frame and the scheduler's own frames come on top
of all three.

Two consequences follow when sizing a thread. `stack_budget` bounds the
caller's part table, and a part's size changes an agent's frame only
through the nesting above, never through the depth of the tree. And the
cheapest agent on a small device is the fold, which is what it exists for.

A worked case, a Zephyr port: a scalar-only build
(`BLAKE3PP_MAX_SIMD_DEGREE=1`) hashing 1 MiB with `stack_budget{1024}`, so
parts of 32 chunks. The caller needs 1 KiB for the table. The message fits
one window, so each agent runs the whole-subtree form: 2736 bytes, or 720
with `BLAKE3PP_SUBTREE_FOLD=12`. A longer message's full windows cost an
agent both forms, 1024 + 2736 bytes, or 352 + 720 with the fold.

These are one toolchain's frames; another compiler, ABI or optimization
level will differ. Measure yours the same way: compile `src/blake3pp.cpp`
with `-fstack-usage`, read the `.su` entries for the functions named
above, and leave the thread its own headroom on top.
