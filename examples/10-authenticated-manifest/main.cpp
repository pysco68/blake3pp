// Three choices that compose: keyed mode, file input, and verification.
// Together they are an authenticated manifest -- a checksum list that
// only the holder of the key can produce or trust.

#include <blake3pp/blake3pp.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::array<std::byte, blake3pp::key_size> demo_key() {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(0xA0 + i);
  }
  return key;
}

void write_file(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

}  // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "blake3pp-example-10";
  std::filesystem::create_directories(dir);
  for (const auto& [name, text] : {std::pair{"alpha.txt", "the first payload"},
                                   std::pair{"beta.txt", "the second payload"},
                                   std::pair{"gamma.txt", "the third payload"}}) {
    write_file(dir / name, text);
  }

  const auto key = demo_key();
  const std::vector<std::string> names = {"alpha.txt", "beta.txt", "gamma.txt"};

  // Building the manifest: a keyed hash per file. The key rides in the
  // options, so this is still one call per file.
  std::vector<std::pair<std::string, std::string>> manifest;
  std::cout << "manifest\n";
  for (const auto& name : names) {
    const auto tag = blake3pp::hash_file(dir / name, {.key = key});
    manifest.emplace_back(name, tag.to_hex());
    std::cout << "  " << tag.to_hex() << "  " << name << '\n';
  }

  // Verifying it: recompute and compare. matches() does both steps, in
  // constant time, and treats unparseable hex as a mismatch rather than
  // an error.
  std::cout << "\nverifying\n";
  for (const auto& [name, expected] : manifest) {
    const auto actual = blake3pp::hash_file(dir / name, {.key = key});
    std::cout << "  " << name << ": "
              << (actual.matches(expected) ? "ok" : "FAILED") << '\n';
  }

  // A modified file fails, which a plain checksum would also catch...
  write_file(dir / "beta.txt", "the second payload, tampered with");
  const auto tampered = blake3pp::hash_file(dir / "beta.txt", {.key = key});
  std::cout << "\nafter editing beta.txt: "
            << (tampered.matches(manifest[1].second) ? "ok" : "FAILED") << '\n';

  // ...and so does a manifest rewritten by someone without the key, which
  // a plain checksum would not. Without the key, no correct line can be
  // produced for the new contents.
  const auto forged = blake3pp::hash_file(dir / "beta.txt");   // unkeyed
  std::cout << "a line computed without the key: "
            << (forged.matches(tampered.to_hex()) ? "accepted" : "rejected")
            << '\n';

  std::filesystem::remove_all(dir);
}
