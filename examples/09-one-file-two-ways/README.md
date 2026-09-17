# One file, two ways

Two independent choices, combined: **where the bytes come from** and
**how many cores hash them**. Going multi-core over a file adds a
scheduler argument and changes nothing else, including the digest.

```cpp
auto one  = blake3pp::hash_file(path);           // one core
auto many = blake3pp::hash_file(path, sched);    // every core
assert(one == many);
```

This is the one example without a Compiler Explorer link. Its subject is
storage and cores, and a sandbox has neither: the file it wants to write
exceeds the size limit there, and the core count would not show a
speedup anyway. Run it on your own disk.

Both calls read the same way: asynchronously, bypassing the page cache,
with several windows in flight. The sequential one is not a slow path
with the I/O switched off — what is sequential is the hashing, not the
reading.

## What the numbers mean

The speedup is bounded by whichever of the two runs out first, the cores
or the device. On a fast NVMe stripe the hashing stops being the limit
and the rates converge; on a slower disk they converge sooner. That is
the point of the pipeline rather than a shortcoming of it.

When the device is the limit, the window size and queue depth are the
knobs that move it, in either direction:

```cpp
blake3pp::hash_file(path, sched, {.window_bytes = 16 * 1024 * 1024,
                                  .queue_depth  = 8});
```

Bigger windows and more reads in flight pay off on a fast NVMe stripe and
can cost you on a slow or virtualised filesystem, where they just add
latency to each window. The example prints that third measurement rather
than recommending a number, because the right one is a property of your
storage. The digest never changes across any of them.

## Running it

With no arguments it writes a 32 MiB file to the temporary directory and
hashes that. Given a path, it hashes that file instead, which is the more
interesting measurement.

```
cmake --build build --target blake3pp_example_09_one_file_two_ways
./build/examples/blake3pp_example_09_one_file_two_ways [path]
```

## Next

- [10-authenticated-manifest](../10-authenticated-manifest/) combines
  keying with files.
