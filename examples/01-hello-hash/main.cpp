// One-shot hashing, and what a digest is.
//
// Build this on its own with:
//     c++ -std=c++20 -I../../include main.cpp -L. -lblake3pp

#include <blake3pp/blake3pp.hpp>

#include <cstddef>
#include <iostream>
#include <optional>
#include <vector>

int main() {
  // The simplest call there is. A string_view goes straight in.
  const blake3pp::digest d = blake3pp::hash("hello world");
  std::cout << "hash(\"hello world\")\n  " << d.to_hex() << "\n\n";

  // Anything span-like works the same way.
  const std::vector<std::byte> payload(4096, std::byte{0x42});
  std::cout << "hash(4 KiB of 0x42)\n  "
            << blake3pp::hash(payload).to_hex() << "\n\n";

  // A digest is a regular value type, so it compares and copies like one.
  std::cout << std::boolalpha;
  std::cout << "the same input hashes to the same digest: "
            << (d == blake3pp::hash("hello world")) << '\n';

  // It also round-trips through hex.
  const std::optional<blake3pp::digest> parsed =
      blake3pp::digest::from_hex(d.to_hex());
  std::cout << "and survives a trip through hex: " << (parsed == d) << '\n';

  // Checking a digest that arrived from somewhere else is one call. The
  // comparison is constant time, and hex that does not parse is no match
  // rather than an error.
  std::cout << "matches() accepts the right hex:  "
            << d.matches("d74981efa70a0c880b8d8c1985d075dbcbf679b99a5f9914e5aaf96b831a9e24")
            << '\n';
  std::cout << "matches() rejects anything else:  "
            << d.matches("not hex at all") << '\n';

  // For hot paths and C interop there is an allocation-free hex form: 64
  // characters plus a terminating NUL.
  const std::array<char, 65> hex = d.to_hex_chars();
  std::cout << "\nto_hex_chars() does not allocate: " << hex.data() << '\n';
}
