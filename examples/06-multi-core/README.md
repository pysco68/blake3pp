# Multi-core hashing

```cpp
auto sched = blake3pp::get_parallel_scheduler();
blake3pp::digest d = blake3pp::hash(big_buffer, sched);
```

**[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/06-multi-core/)** -- it runs there, with no setup.

Adding cores means adding a scheduler argument. Nothing else about the
call changes, and the digest does not change either.

## Why the digest is identical

BLAKE3 hashes a binary Merkle tree. Any power-of-two, position-aligned
run of chunks reduces to one chaining value independently of everything
around it, so the parallel decomposition is exact rather than
approximate. The input is split into equal subtrees, the scheduler's
agents take them as they come free, and the chaining values are absorbed
in order. The result is the sequential result.

## Where the scheduler comes from

`get_parallel_scheduler()` returns the process-wide scheduler in P2079's
shape, which is the same call C++26 application code makes. Anyone who
needs a sized or bounded pool builds their provider's pool and passes its
scheduler instead:

```cpp
exec::static_thread_pool pool(8);
auto d = blake3pp::hash(big_buffer, pool.get_scheduler());
```

Every entry point takes a scheduler the same way. The sequential and
multi-core spellings sit side by side:

|             | one-shot            | incremental       | file input                      | extended output       |
|-------------|---------------------|-------------------|---------------------------------|-----------------------|
| sequential  | `hash(data)`        | `hasher`          | `update_file(h, path)`          | `fill(r, out)`        |
| multi-core  | `hash(data, sched)` | `parallel_hasher` | `update_file(h, path, sched)`   | `fill(r, out, sched)` |

## Stack, and the budget that caps it

Each part's 32-byte chaining value waits on the calling thread's stack.
`stack_budget` is a template argument that caps how much stack that table
may take, defaulting to 32 KiB, which is 1024 parts:

```cpp
auto d = blake3pp::hash<blake3pp::stack_budget{1024}>(buffer, sched);   // 32 parts
```

A budget that is not a multiple of 32 bytes, or that holds fewer than two
parts, does not compile.

The budget covers that table and nothing else. Each part is reduced on
the agent that took it, by a recursion holding one chaining-value buffer
per level, so the agent threads need stack of their own. The two costs
pull against each other: a smaller budget makes each part larger, which
makes that recursion deeper.
[Freestanding and RTOS builds](../../docs/freestanding.md) works the
numbers through.

## Running it

```
cmake --build build --target blake3pp_example_06_multi_core
./build/examples/blake3pp_example_06_multi_core
```

It hashes 32 MiB both ways and prints the rate of each, so the output
depends on the machine.

## Next

- [07-which-kernel](../07-which-kernel/) asks the binary what it carries
  and the CPU what it can run.
