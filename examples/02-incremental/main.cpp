// Hashing data that arrives in pieces, and taking a digest without
// giving up the hasher.

#include <blake3pp/blake3pp.hpp>

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

int main() {
  const std::string_view part1 = "the quick brown fox ";
  const std::string_view part2 = "jumps over the lazy dog";

  // A hasher accumulates. It is a fixed-size value type and never
  // allocates, so it is as cheap to keep around as the buffer you feed it.
  blake3pp::hasher h;
  h.update(part1);
  h.update(part2);

  const blake3pp::digest streamed = h.finalize();
  std::cout << "hashed in two updates\n  " << streamed.to_hex() << '\n';

  // The result is the digest of the concatenation, as if it had gone in
  // as one message.
  std::string joined{part1};
  joined += part2;
  std::cout << "\nthe same bytes in one call\n  "
            << blake3pp::hash(joined).to_hex() << '\n';
  std::cout << "\nidentical: " << std::boolalpha
            << (streamed == blake3pp::hash(joined)) << '\n';

  // finalize() does not consume the hasher. You can take a digest of
  // everything so far and carry on feeding it, which is what makes
  // checkpointing a long stream possible.
  h.update(" -- and then some more");
  const blake3pp::digest longer = h.finalize();
  std::cout << "\nafter one more update\n  " << longer.to_hex() << '\n';

  // reset() returns the hasher to its initial state so the instance can
  // be reused for the next message.
  h.reset();
  h.update("a fresh message");
  std::cout << "\nafter reset(), a new message\n  "
            << h.finalize().to_hex() << '\n';

  // Binary input goes in as a span of bytes.
  const std::vector<std::byte> block(1024, std::byte{0x01});
  blake3pp::hasher b;
  for (int i = 0; i < 16; ++i) {
    b.update(block);
  }
  std::cout << "\n16 KiB fed 1 KiB at a time\n  " << b.finalize().to_hex() << '\n';
}
