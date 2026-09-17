# One file, two ways

Two independent choices, combined: **where the bytes come from** and
**how many cores hash them**. Going multi-core over a file adds a
scheduler argument and changes nothing else, including the digest.

[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/09-one-file-two-ways/)

```cpp
auto one  = blake3pp::hash_file(path);           // one core
auto many = blake3pp::hash_file(path, sched);    // every core
assert(one == many);
```

Both calls read the same way: asynchronously, bypassing the page cache,
with several windows in flight. The sequential one is not a slow path
with the I/O switched off — what is sequential is the hashing, not the
reading.

## Tuning the pipeline

The window size and the number of reads in flight are where the pipeline
is fitted to a device:

```cpp
blake3pp::hash_file(path, sched, {.window_bytes = 4 * 1024 * 1024,
                                  .queue_depth  = 8});
```

Two things worth knowing before reaching for them:

- The window has to divide the file into more than one piece for any of it
  to matter. A window larger than the file leaves one window, and one
  window cannot overlap with anything.
- Which values help is a property of your storage. Bigger windows and
  more reads in flight pay off on a fast NVMe stripe and can cost you on a
  slow or virtualised filesystem, where they add latency to each window.

Neither knob can change the answer, which is what the example checks.

## Where the ceiling is

The speedup you get is bounded by whichever runs out first, the cores or
the device. On a fast NVMe stripe the hashing stops being the limit and
the two rates converge. On a slower disk they converge sooner. Reaching
the device's ceiling is what the pipeline is for.

This example prints digests rather than rates, because a rate measured
once on an arbitrary filesystem says more about the filesystem than about
the library. `blake3pp_bench_file` is the tool for that question: it
measures the device ceiling first, with the pipeline delivering windows
that are released unread, and reports every hashing row as a fraction
of it.

## Running it

With no arguments it writes a 12 MiB file to the temporary directory and
hashes that. The size is deliberate: it is larger than the default 8 MiB
window, so the parallel path has more than one window to work with, and
small enough for Compiler Explorer, where files are capped at 16 MiB.

Given a path, it hashes that file instead.

```
cmake --build build --target blake3pp_example_09_one_file_two_ways
./build/examples/blake3pp_example_09_one_file_two_ways [path]
```

## Next

- [10-authenticated-manifest](../10-authenticated-manifest/) combines
  keying with files.
