// BLAKE3's two secret-key modes: keyed hashing as a MAC, and derive_key
// as a KDF.

#include <blake3pp/blake3pp.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>

namespace {

// A stand-in for a key that would really come from a key store. Keys are
// exactly 32 bytes, and the span extent in the signatures turns a
// wrong-sized key into a compile error.
std::array<std::byte, blake3pp::key_size> demo_key(std::byte seed) {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(static_cast<unsigned>(seed) + i);
  }
  return key;
}

}  // namespace

int main() {
  const std::array<std::byte, blake3pp::key_size> key = demo_key(std::byte{0x11});
  const std::string_view message = "transfer 100 to account 12345";

  // Keyed mode is BLAKE3's built-in MAC, and the modern replacement for
  // HMAC: no nested construction, no separate key schedule.
  const blake3pp::digest tag = blake3pp::keyed_hash(key, message);
  std::cout << "keyed_hash(key, message)\n  " << tag.to_hex() << '\n';

  // Under a different key the same message authenticates differently.
  std::cout << "\nunder another key\n  "
            << blake3pp::keyed_hash(demo_key(std::byte{0x22}), message).to_hex()
            << '\n';

  // The keyed mode is a hasher like any other, so a long or piecewise
  // message authenticates incrementally.
  blake3pp::hasher mac = blake3pp::hasher::keyed(key);
  mac.update("transfer 100 ");
  mac.update("to account 12345");
  std::cout << "\nthe same tag, built incrementally\n  "
            << mac.finalize().to_hex() << '\n';
  std::cout << "\nidentical: " << std::boolalpha
            << (mac.finalize() == tag) << '\n';

  // derive_key is the domain-separated KDF. The context string is not a
  // secret. It is what keeps subkeys of one master secret independent of
  // each other, so it should be hardcoded and unique to its purpose.
  const std::array<std::byte, blake3pp::key_size> master = demo_key(std::byte{0x99});

  const blake3pp::digest session_key =
      blake3pp::derive_key("example.com 2026-09 tls session", master);
  const blake3pp::digest storage_key =
      blake3pp::derive_key("example.com 2026-09 disk encryption", master);

  std::cout << "\nderive_key, two purposes from one master secret\n"
            << "  tls session      " << session_key.to_hex() << '\n'
            << "  disk encryption  " << storage_key.to_hex() << '\n';
  std::cout << "\nunrelated to each other: "
            << (session_key != storage_key) << '\n';

  // A derived key can be wider than 32 bytes, because BLAKE3's output is
  // a stream. 04-extended-output covers that.
  blake3pp::hasher kdf = blake3pp::hasher::derive_key("example.com 2026-09 aead");
  kdf.update(master);
  const std::array<std::byte, 64> wide = kdf.finalize<64>();
  std::cout << "\na 64-byte derived key\n  " << blake3pp::to_hex(wide) << '\n';
}
