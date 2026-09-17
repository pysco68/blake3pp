# Keyed hashing and extended output

Two independent choices, combined: **which key** and **how much output**.
A MAC is not fixed at 32 bytes, and a derived key is not fixed at one
width.

[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/08-keyed-and-extended-output/)

```cpp
blake3pp::hasher mac = blake3pp::hasher::keyed(key);
mac.update(message);

auto tag32 = mac.finalize();          // the usual 32-byte tag
auto tag64 = mac.finalize<64>();      // the same stream, read further
auto keystream = mac.finalize_xof();  // ...or all of it, seekable
```

A keyed hasher is a hasher. Every finalize form in
[04-extended-output](../04-extended-output/) applies to it unchanged, so
the 32-byte tag is the first 32 bytes of an authenticated stream.

Keying changes the whole stream, not a prefix of it. The unkeyed hash of
the same message shares no bytes with the keyed one at any offset.

## Why this combination is useful

- **A wider MAC.** Where 32 bytes is not the tag size a protocol wants,
  take the width it does want.
- **An authenticated keystream.** `finalize_xof()` on a keyed hasher is
  seekable, so a consumer can start at any offset without generating
  what precedes it.
- **Subkeys of different widths from one context.** `derive_key` yields a
  stream too, so a 32-byte AEAD key and a 16-byte header key can come
  from one derivation at two offsets.

## Running it

```
cmake --build build --target blake3pp_example_08_keyed_and_extended_output
./build/examples/blake3pp_example_08_keyed_and_extended_output
```

## Next

- [09-one-file-two-ways](../09-one-file-two-ways/) combines a file with a
  scheduler.
