# Official BLAKE3 test vectors

`test_vectors.json` is copied verbatim from the reference repository:
<https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json>
(CC0-1.0 / Apache-2.0 dual licensed, like the rest of that repository).

Each case's input is `input_len` bytes of the repeating byte pattern
`0, 1, ..., 250, 0, 1, ...`. The `hash`, `keyed_hash` and `derive_key` fields
are 131 bytes of extended (XOF) output in hex; the first 64 hex characters
are the standard 32-byte output. The JSON is kept as-is (no comment header,
since JSON forbids comments) and turned into a header at build time by
`tools/gen-test-vectors.py`.
