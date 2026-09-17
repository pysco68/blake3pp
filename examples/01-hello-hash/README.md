# One-shot hashing

The shortest path from bytes to a digest, and what you can do with the
digest once you have it.

**[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/01-hello-hash/)** -- it runs there, with no setup.

```cpp
blake3pp::digest d = blake3pp::hash("hello world");
std::cout << d.to_hex() << '\n';
```

`blake3pp::hash` takes a `std::string_view` or anything convertible to
`std::span<const std::byte>`, so strings, vectors and arrays all go in
without a cast.

`blake3pp::digest` is a value type. It compares with `==`, copies, and
round-trips through hex:

- `to_hex()` returns a `std::string` of 64 lowercase characters.
- `to_hex_chars()` returns a `std::array<char, 65>` and never allocates.
- `digest::from_hex()` parses, returning `std::optional<digest>`.
- `matches()` compares against hex in one step.

Comparison is constant time in every form, which is the safe default for
a value that gets compared against untrusted input. Hex that does not
parse is treated as "no match" rather than as an error.

## Running it

```
cmake --build build --target blake3pp_example_01_hello_hash
./build/examples/blake3pp_example_01_hello_hash
```

## Next

- [02-incremental](../02-incremental/) hashes data that arrives in pieces.
