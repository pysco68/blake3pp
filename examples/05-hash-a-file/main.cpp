// Hashing files. The pipeline uses the platform's own async read
// mechanism and bypasses the page cache where it can.

#include <blake3pp/blake3pp.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

namespace {

// Two files to hash, so the example runs anywhere without arguments.
std::filesystem::path write_demo_file(const std::filesystem::path& dir,
                                      const char* name, char fill,
                                      std::size_t bytes) {
  const std::filesystem::path path = dir / name;
  std::ofstream out(path, std::ios::binary);
  const std::vector<char> block(64 * 1024, fill);
  for (std::size_t written = 0; written < bytes; written += block.size()) {
    out.write(block.data(), static_cast<std::streamsize>(block.size()));
  }
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  // Hash whatever the caller named, or a pair of generated files.
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::cout << blake3pp::hash_file(argv[i]).to_hex() << "  " << argv[i]
                << '\n';
    }
    return 0;
  }

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "blake3pp-example-05";
  std::filesystem::create_directories(dir);
  const std::filesystem::path a = write_demo_file(dir, "a.bin", 'a', 4 << 20);
  const std::filesystem::path b = write_demo_file(dir, "b.bin", 'b', 1 << 20);

  // The one-shot form. Paths are std::filesystem::path, and this
  // overload throws std::system_error on an I/O failure.
  std::cout << "hash_file(a.bin)\n  " << blake3pp::hash_file(a).to_hex() << '\n';

  // Every entry point also has a std::error_code form, mirroring the
  // standard library, for callers who would rather branch than catch.
  std::error_code ec;
  const blake3pp::digest d = blake3pp::hash_file(dir / "does-not-exist", ec);
  if (ec) {
    std::cout << "\nhash_file() on a missing file reports\n  " << ec.message()
              << '\n';
  }

  // update_file() is the primitive underneath: it streams a file into a
  // hasher you own. Because the hasher is yours, its mode and all of its
  // finalize forms apply to file input, and several files hash as one
  // message.
  blake3pp::hasher both;
  blake3pp::update_file(both, a);
  blake3pp::update_file(both, b);
  std::cout << "\nboth files as one message\n  " << both.finalize().to_hex()
            << '\n';

  // The same file through a keyed hasher gives an authenticated manifest
  // entry rather than a plain checksum.
  const std::array<std::byte, blake3pp::key_size> key{};
  blake3pp::hasher mac = blake3pp::hasher::keyed(key);
  blake3pp::update_file(mac, a);
  std::cout << "\na keyed hash of a.bin\n  " << mac.finalize().to_hex() << '\n';

  std::filesystem::remove_all(dir);
  (void)d;
}
