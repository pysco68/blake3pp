# Hashing a file

```cpp
blake3pp::digest d = blake3pp::hash_file("dataset.parquet");
```

That call opens the file with the fastest mechanism the platform offers,
keeps several reads in flight, and hashes each window as it lands.

| OS | async engine | page-cache bypass |
|----|--------------|-------------------|
| Linux | io_uring | `O_DIRECT` |
| Windows | IOCP | `FILE_FLAG_NO_BUFFERING` |
| macOS | GCD (libdispatch) | `F_NOCACHE` |

Each of those degrades on its own at run time. Without direct I/O the
pipeline falls back to buffered async reads, then to synchronous reads,
then to stdio. Nothing needs configuring for that to happen.

## Throwing or not

Every entry point comes in two forms, following the standard library:

```cpp
auto d  = blake3pp::hash_file(path);        // throws std::system_error

std::error_code ec;
auto d2 = blake3pp::hash_file(path, ec);    // reports through ec
```

Paths are `std::filesystem::path`. A path-like type from another library
is accepted structurally if it has a `native()` observer, so a
Boost.Filesystem path compiles without blake3pp knowing Boost exists.

## Feeding a hasher instead

`hash_file()` is the one-shot form. `update_file()` is the primitive
under it, and it streams a file into a hasher you own:

```cpp
blake3pp::hasher all;
for (const auto& part : parts) {
  blake3pp::update_file(all, part);
}
auto d = all.finalize();
```

Because the hasher belongs to the caller, its mode and all of its
finalize forms apply to file input. That makes a keyed hash of a file, or
a seekable stream derived from a file's bytes, the same one-liner.

## Running it

With no arguments it generates two files in the temporary directory and
hashes those. Given paths, it hashes them instead.

```
cmake --build build --target blake3pp_example_05_hash_a_file
./build/examples/blake3pp_example_05_hash_a_file [paths...]
```

## Next

- [06-multi-core](../06-multi-core/) adds a scheduler, which is what
  takes the file pipeline to full speed.
