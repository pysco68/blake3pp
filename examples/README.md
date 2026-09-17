# Examples

Each directory is one program with one subject. They build and run on
their own, and each has a README explaining what it shows.

| | | |
|---|---|---|
| [01-hello-hash](01-hello-hash/) | one-shot hashing, and what a `digest` is | [try it](https://pysco68.github.io/blake3pp/try/01-hello-hash/) |
| [02-incremental](02-incremental/) | `hasher`, for input that arrives in pieces | [try it](https://pysco68.github.io/blake3pp/try/02-incremental/) |
| [03-keyed-and-derive-key](03-keyed-and-derive-key/) | the MAC mode and the KDF mode | [try it](https://pysco68.github.io/blake3pp/try/03-keyed-and-derive-key/) |
| [04-extended-output](04-extended-output/) | output of any length, and O(1) seeking into it | [try it](https://pysco68.github.io/blake3pp/try/04-extended-output/) |
| [05-hash-a-file](05-hash-a-file/) | `hash_file`, `update_file`, and the two error conventions | [try it](https://pysco68.github.io/blake3pp/try/05-hash-a-file/) |
| [06-multi-core](06-multi-core/) | a scheduler, and why the digest does not change | [try it](https://pysco68.github.io/blake3pp/try/06-multi-core/) |
| [07-which-kernel](07-which-kernel/) | what the binary carries, what the CPU can run | [try it](https://pysco68.github.io/blake3pp/try/07-which-kernel/) |

Read them in order for a tour of the library, or jump to the one that
matches the problem in front of you.

## Building them

They are on by default when blake3pp is the top-level project:

```
cmake --preset linux-gcc16-cxx26
cmake --build build/linux-gcc16-cxx26
ctest --test-dir build/linux-gcc16-cxx26 -R blake3pp_example
```

Each one is also a test, so `ctest` running them is what keeps them
describing the library as it currently is. To leave them out of a build,
configure with `-DBLAKE3PP_BUILD_EXAMPLES=OFF`.

## One file that is not in this list

`parallel-demo.cpp` is the program behind the
["try it" link](https://pysco68.github.io/blake3pp/try/). Compiler
Explorer can only run a single translation unit, so `website/amalgamate.py`
concatenates the library into one file and appends a program to it. That
file is generated when the site is built and is not kept here.
