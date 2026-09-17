# Incremental hashing

Data that does not arrive all at once still hashes in one pass.

```cpp
blake3pp::hasher h;
while (auto block = source.next_block()) {
  h.update(*block);
}
blake3pp::digest d = h.finalize();
```

`hasher` is a fixed-size value type and never allocates, so it costs no
more to hold than the buffer feeding it. `update()` accepts a
`std::string_view` or a `std::span<const std::byte>`.

The digest of a message fed in pieces equals the digest of the same
bytes passed to `blake3pp::hash()` in one call. How the input is
chunked never changes the result.

Two things are worth knowing about `finalize()`:

- It is non-destructive. It returns the digest of everything fed so far
  and leaves the hasher able to accept more, which is how you checkpoint
  a long stream.
- It is `const`. Taking a digest is a read of the hasher's state.

`reset()` returns the hasher to its initial state, so one instance can
hash message after message.

## Running it

```
cmake --build build --target blake3pp_example_02_incremental
./build/examples/blake3pp_example_02_incremental
```

## Next

- [03-keyed-and-derive-key](../03-keyed-and-derive-key/) for the MAC and
  the key-derivation modes.
- [05-hash-a-file](../05-hash-a-file/) feeds a hasher from a file.
