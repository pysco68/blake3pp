// ---------------------------------------------------------------------
// The demo. Delete from here down to use the file as a library.
//
// C++20, like the library itself: no <print>, so every toolchain the
// amalgamation supports can also build the file it is pasted into. The
// published try links are exactly this.
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {
void line(const std::string& text) {
  std::fputs(text.c_str(), stdout);
  std::fputc('\n', stdout);
}
}  // namespace

int main() {
  std::string carried;
  for (auto a : blake3pp::compiled_arches()) {
    carried += blake3pp::to_string(a);
    carried += ' ';
  }
  line("blake3pp " + std::string{blake3pp::version()} + " · simd " +
       std::string{blake3pp::simd_provider()} + " · carries " + carried +
       "· auto picks " +
       std::string{blake3pp::to_string(blake3pp::hasher{}.selected_arch())});

  // One shot, and the digest as hex.
  line("\"abc\"      " + blake3pp::hash("abc").to_hex());

  // Incremental: finalize() does not consume the hasher.
  blake3pp::hasher h;
  h.update("a");
  h.update("bc");
  line("streamed   " + h.finalize().to_hex());

  // Keyed, for a MAC, and a context-separated subkey.
  std::array<std::byte, 32> key{};
  line("keyed      " + blake3pp::keyed_hash(key, "abc").to_hex());
  line("derived    " + blake3pp::derive_key("example 2026", "abc").to_hex());

  // Extended output, seekable in constant time.
  auto r = blake3pp::hasher{}.finalize_xof();
  r.seek(1'000'000'000);
  auto far = r.take<16>();
  line("byte 1e9   " + blake3pp::to_hex(far));
}
