# Extended output

BLAKE3 is natively an extendable-output function. The 32-byte digest is
the first 32 bytes of an unbounded stream, and the rest of it costs no
more to reach.

**[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/04-extended-output/)** -- it runs there, with no setup.

## Asking for a width

```cpp
auto wide = h.finalize<64>();          // std::array<std::byte, 64>

std::vector<std::byte> out(n);
h.finalize(out);                       // any runtime length
```

The compile-time form returns by value. The span form fills a buffer the
caller owns, which is what you want for a length known only at run time.

## Taking the reader

```cpp
blake3pp::output_reader r = h.finalize_xof();
r.fill(chunk);                         // stream sequentially
auto next = r.take<32>();              // the same, by value
r.seek(10'000'000'000);                // or jump anywhere
```

Seeking is O(1). Each 64-byte block of the stream is one compression
carrying its own counter, so it depends on nothing that came before it.
Byte ten billion costs exactly what byte zero costs, and seeking back
yields the same bytes again.

That independence is also why the stream parallelizes: with a scheduler,
`blake3pp::fill(r, buffer, sched)` splits one request into segments that
fill on every core, each straight into its own slice of the buffer. It is
what `blake3ppgen --threads` runs on.

Extended output works in all three modes. A keyed hasher streams
authenticated output, and a `derive_key` hasher emits subkeys of any
width.

## Running it

```
cmake --build build --target blake3pp_example_04_extended_output
./build/examples/blake3pp_example_04_extended_output
```

## Next

- [05-hash-a-file](../05-hash-a-file/) reads the input from storage.
- [06-multi-core](../06-multi-core/) spreads the work over cores.
