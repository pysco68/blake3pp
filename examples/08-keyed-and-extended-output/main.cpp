// Two choices that compose: a keying mode, and how much output you take.
// Neither constrains the other, so a MAC can be any width and a derived
// key can be a stream.

#include <blake3pp/blake3pp.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {

std::array<std::byte, blake3pp::key_size> demo_key(std::byte seed) {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(static_cast<unsigned>(seed) + i);
  }
  return key;
}

}  // namespace

int main() {
  const auto key = demo_key(std::byte{0x40});
  const std::string_view message = "the message under authentication";

  // A keyed hasher is a hasher, so every finalize form works on it. The
  // 32-byte tag is just the first 32 bytes of what it can produce.
  blake3pp::hasher mac = blake3pp::hasher::keyed(key);
  mac.update(message);

  std::cout << "keyed, 32 bytes\n  " << mac.finalize().to_hex() << "\n\n";
  std::cout << "keyed, 64 bytes\n  " << blake3pp::to_hex(mac.finalize<64>())
            << "\n\n";
  std::cout << std::boolalpha
            << "the 32-byte tag is that stream's first 32 bytes: "
            << (blake3pp::to_hex(mac.finalize<64>()).substr(0, 64)
                == mac.finalize().to_hex())
            << '\n';

  // Keying changes the whole stream, not only the first 32 bytes.
  blake3pp::hasher plain;
  plain.update(message);
  std::cout << "\nunkeyed, the same message, 64 bytes\n  "
            << blake3pp::to_hex(plain.finalize<64>()) << '\n';

  // An authenticated keystream: seekable, so a consumer can start
  // anywhere without generating what comes before.
  blake3pp::output_reader stream = mac.finalize_xof();
  std::cout << "\nkeyed keystream\n"
            << "  bytes 0-31       " << blake3pp::to_hex(stream.take<32>())
            << '\n';
  stream.seek(1'000'000);
  std::cout << "  bytes 1e6..+32   " << blake3pp::to_hex(stream.take<32>())
            << '\n';

  // derive_key composes the same way, which is how one master secret
  // yields subkeys of whatever width each use needs.
  blake3pp::hasher kdf =
      blake3pp::hasher::derive_key("example.com 2026-09 channel keys");
  kdf.update(demo_key(std::byte{0x90}));

  const auto aead_key = kdf.finalize<32>();   // for a 256-bit AEAD
  blake3pp::output_reader subkeys = kdf.finalize_xof();
  subkeys.seek(32);                           // ...and the next one after it
  const auto header_key = subkeys.take<16>();

  std::cout << "\nderive_key, two widths from one context\n"
            << "  32-byte AEAD key  " << blake3pp::to_hex(aead_key) << '\n'
            << "  16-byte header key " << blake3pp::to_hex(header_key) << '\n';
}
