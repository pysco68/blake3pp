# Keyed hashing and key derivation

BLAKE3 has two secret-key modes built into the same function.

**[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/03-keyed-and-derive-key/)** -- it runs there, with no setup.

## Keyed hashing: the MAC

```cpp
blake3pp::digest tag = blake3pp::keyed_hash(key, message);
```

This is BLAKE3's message authentication code, and it replaces HMAC
directly. There is no nested construction and no separate key schedule:
the key simply replaces the initialization vector.

Keys are exactly 32 bytes. The signatures take
`std::span<const std::byte, 32>`, so a wrong-sized key fails to compile
instead of failing at run time.

For a message that arrives in pieces, `hasher::keyed(key)` returns a
hasher in keyed mode, and everything in
[02-incremental](../02-incremental/) applies to it.

## derive_key: the KDF

```cpp
auto session = blake3pp::derive_key("example.com 2026-09 tls session", master);
auto storage = blake3pp::derive_key("example.com 2026-09 disk encryption", master);
```

`derive_key` produces purpose-bound subkeys from one master secret. The
context string is not a secret, and it is not a salt either: it is the
domain separator that makes unrelated uses of the same key material
cryptographically independent. Hardcode it, and make it unique to the
purpose. Application name, date and use are a good shape.

Both modes compose with the rest of the library. A keyed hasher accepts
file input through `update_file()`, goes multi-core through
`parallel_hasher`, and emits output of any width through
`finalize_xof()`.

## Running it

```
cmake --build build --target blake3pp_example_03_keyed_and_derive_key
./build/examples/blake3pp_example_03_keyed_and_derive_key
```

## Next

- [04-extended-output](../04-extended-output/) derives keys wider than 32
  bytes.
