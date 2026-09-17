
// ---------------------------------------------------------------------
// The demo. Delete from here down to use the file as a library.
#include <array>
#include <print>
#include <string>
#include <vector>

int main() {
  std::string carried;
  for (auto a : blake3pp::compiled_arches()) {
    carried += blake3pp::to_string(a);
    carried += ' ';
  }
  std::println("blake3pp {} · simd {} · carries {}· auto picks {}",
               blake3pp::version(), blake3pp::simd_provider(), carried,
               blake3pp::to_string(blake3pp::hasher{}.selected_arch()));

  // One shot, and the digest as hex.
  std::println("\"abc\"      {}", blake3pp::hash("abc").to_hex());

  // Incremental: finalize() does not consume the hasher.
  blake3pp::hasher h;
  h.update("a");
  h.update("bc");
  std::println("streamed   {}", h.finalize().to_hex());

  // Keyed, for a MAC, and a context-separated subkey.
  std::array<std::byte, 32> key{};
  std::println("keyed      {}", blake3pp::keyed_hash(key, "abc").to_hex());
  std::println("derived    {}", blake3pp::derive_key("example 2026", "abc").to_hex());

  // Extended output, seekable in constant time.
  auto r = blake3pp::hasher{}.finalize_xof();
  r.seek(1'000'000'000);
  auto far = r.take<16>();
  std::println("byte 1e9   {}", blake3pp::to_hex(far));
}
