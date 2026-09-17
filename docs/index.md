# blake3pp

BLAKE3 for C++20 and later. One line gets you a hash. The fast paths are
there when you need them, and you do not have to ask for them twice.

## Your first hash

```cpp
#include <blake3pp/blake3pp.hpp>
#include <iostream>

int main() {
  std::cout << blake3pp::hash("hello world").to_hex() << '\n';
}
```

```
d74981efa70a0c880b8d8c1985d075dbcbf679b99a5f9914e5aaf96b831a9e24
```

`hash()` takes a string or anything span-like, and hands back a
`digest`: a value type you can compare, copy, and print as hex.

## Hashing a file

Files are a first-class input, not a read loop you write yourself:

```cpp
blake3pp::digest d = blake3pp::hash_file("dataset.parquet");
```

That opens the file with the fastest mechanism your platform has --
io_uring on Linux, IOCP on Windows, GCD on macOS -- bypasses the page
cache, and keeps several reads in flight while it hashes. If any of that
is unavailable it quietly steps down to something that works.

## Using every core

Add a scheduler:

```cpp
auto sched = blake3pp::get_parallel_scheduler();
blake3pp::digest d = blake3pp::hash(big_buffer, sched);
```

The digest is the same one you would get sequentially. BLAKE3 hashes a
tree, so splitting the work is exact rather than approximate, and every
entry point takes a scheduler the same way.

## Getting it into your build

```cmake
include(FetchContent)
FetchContent_Declare(blake3pp
  GIT_REPOSITORY https://github.com/pysco68/blake3pp.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(blake3pp)
target_link_libraries(your_app PRIVATE blake3pp::blake3pp)
```

One target, and `#include <blake3pp/blake3pp.hpp>` for the whole library.
C++20 is enough: where your toolchain lacks `std::simd` or
`std::execution`, polyfills stand in, and the code you write does not
change. [Integration](integration.md) covers installing, vendoring and
`find_package`.

Want to try it before building anything?
[Run it in your browser](https://pysco68.github.io/blake3pp/try/) on
Compiler Explorer.

## What else is in here

BLAKE3 is more than a 32-byte digest, and all of it is available:

- **Keyed hashing**, BLAKE3's built-in MAC, replacing HMAC directly.
- **`derive_key`**, a KDF with context separation, for purpose-bound
  subkeys from one master secret.
- **Extended output** of any length, seekable in constant time. Byte ten
  billion costs what byte zero costs.
- **Incremental hashing**, sequential or multi-core, with a
  non-destructive `finalize()` so you can checkpoint a stream.

## Where to go next

| | |
|---|---|
| [Examples](examples/index.md) | seven standalone programs, one per subject. Start here if you learn by reading code |
| [The API](api.md) | the guided tour of the whole public surface, with examples |
| [Reference](reference/index.md) | every declaration, generated from the headers' own doc comments |
| [Architectures and kernels](architectures.md) | which SIMD variants a build carries, and how dispatch picks one |
| [Command-line tools](tools.md) | `blake3ppsum` and `blake3ppgen` |
| [Building](building.md) | presets, toolchain images, and the kernel switches |
| [Integration](integration.md) | vendoring, installing, and `find_package` |
| [Freestanding and RTOS builds](freestanding.md) | no OS, no allocator, and a few hundred bytes of stack |

Everything here is Apache-2.0, and the source is
[on GitHub](https://github.com/pysco68/blake3pp).
