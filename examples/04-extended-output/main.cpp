// BLAKE3's output is a stream, not 32 bytes. Read as much of it as you
// want, from wherever you want.

#include <blake3pp/blake3pp.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <vector>

int main() {
  blake3pp::hasher h;
  h.update("seed material for this example");

  // The 32-byte digest is just the first 32 bytes of the stream.
  std::cout << "finalize()        " << h.finalize().to_hex() << '\n';

  // Ask for a compile-time width and get it back by value.
  const std::array<std::byte, 64> wide = h.finalize<64>();
  std::cout << "finalize<64>()    " << blake3pp::to_hex(wide) << '\n';

  // The first 32 bytes of the wider output are the digest: one stream,
  // read to different lengths.
  std::cout << "\nthe digest is the stream's first 32 bytes: " << std::boolalpha
            << (blake3pp::to_hex(wide).substr(0, 64) == h.finalize().to_hex())
            << '\n';

  // A runtime length fills a buffer the caller owns.
  std::vector<std::byte> runtime_sized(200);
  h.finalize(runtime_sized);
  std::cout << "\n200 bytes into a caller's buffer, first 32 shown\n  "
            << blake3pp::to_hex(std::span{runtime_sized}.first(32)) << '\n';

  // For anything longer, take the reader. It streams sequentially...
  blake3pp::output_reader r = h.finalize_xof();
  const std::array<std::byte, 32> first = r.take<32>();
  const std::array<std::byte, 32> second = r.take<32>();
  std::cout << "\nreading the stream in order\n"
            << "  bytes 0-31   " << blake3pp::to_hex(first) << '\n'
            << "  bytes 32-63  " << blake3pp::to_hex(second) << '\n';

  // ...and it seeks in constant time. Byte ten billion costs what byte
  // zero costs, because each 64-byte block of the stream is one
  // compression with its own counter and depends on nothing before it.
  r.seek(10'000'000'000);
  std::cout << "\nafter seek(10'000'000'000)\n  "
            << blake3pp::to_hex(r.take<32>()) << '\n';

  // Seeking back gives the same bytes again: the stream is a function of
  // the input and the offset, not of how it was read.
  r.seek(0);
  std::cout << "\nback at offset 0, the same bytes: "
            << (r.take<32>() == first) << '\n';
}
