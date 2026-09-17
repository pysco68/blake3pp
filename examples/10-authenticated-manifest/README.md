# An authenticated manifest

Three independent choices, combined: **keyed mode**, **file input** and
**verification**. Together they make a checksum list that only a holder
of the key can produce or trust.

[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/10-authenticated-manifest/)

```cpp
auto tag = blake3pp::hash_file(path, {.key = key});   // one line of the manifest
...
if (!recomputed.matches(expected_hex)) { /* reject */ }
```

## What the key buys

A plain checksum list detects accidental corruption. It does not detect a
deliberate edit, because anyone who changes a file can recompute its line
and the list still verifies.

Keying closes that. Each line depends on a secret, so producing a correct
line for altered contents needs the key. The example shows both cases: a
tampered file fails against its old line, and a line computed without the
key is rejected.

This is what `blake3ppsum --keyed` does, and the reason the tool reads
its key from a file rather than from the command line.

## Verification

`matches()` recomputes the comparison against hex in one step. It is
constant-time, like every digest comparison in the library, and hex that
does not parse counts as a mismatch rather than raising. Where you want
the parse to be explicit, `digest::from_hex()` returns a
`std::optional<digest>` that compares directly against a digest.

## Where the key comes from

The key here is generated in the example so that it runs anywhere. A real
one comes from a key store or a key-derivation step — see
[03-keyed-and-derive-key](../03-keyed-and-derive-key/) for `derive_key`,
which turns one master secret into purpose-bound subkeys.

Note that `hash_file_options` carries a key but not a context string:
`derive_key` and extended output have no shortcut there. Build the hasher
yourself and feed it with `update_file()`, as
[05-hash-a-file](../05-hash-a-file/) does.

## Running it

```
cmake --build build --target blake3pp_example_10_authenticated_manifest
./build/examples/blake3pp_example_10_authenticated_manifest
```

## Next

- [The API](../../docs/api.md) lays out every one of these choices and
  what it does not constrain.
