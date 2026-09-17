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

## What the numbers mean

The speedup is bounded by whichever of the two runs out first, the cores
or the device. On a fast NVMe stripe the hashing stops being the limit
and the rates converge. On a slower disk they converge sooner. Reaching
the device's ceiling is what the pipeline is for.

When the device is the limit, the window size and queue depth are the
knobs that move it, in either direction:

```cpp
blake3pp::hash_file(path, sched, {.window_bytes = 4 * 1024 * 1024,
                                  .queue_depth  = 8});
```

The window has to divide the file into more than one piece for any of it
to matter. A window larger than the file leaves one window, and one window
cannot overlap with anything.

Bigger windows and more reads in flight pay off on a fast NVMe stripe and
can cost you on a slow or virtualised filesystem, where they just add
latency to each window. The example prints that third measurement rather
than recommending a number, because the right one is a property of your
storage. The digest never changes across any of them.

## Running it

With no arguments it writes a 12 MiB file to the temporary directory and
hashes that, after one untimed pass so the first measurement is not paying
for the file's first open. Given a path, it hashes that file instead,
which is the more interesting measurement.

The generated size is what Compiler Explorer's sandbox allows: files there
are capped at 16 MiB, and it offers two cores, so the speedup it shows is
small and the device is usually its ceiling. Run it on your own disk for a
number that means something.

```
cmake --build build --target blake3pp_example_09_one_file_two_ways
./build/examples/blake3pp_example_09_one_file_two_ways [path]
```

## Next

- [10-authenticated-manifest](../10-authenticated-manifest/) combines
  keying with files.
