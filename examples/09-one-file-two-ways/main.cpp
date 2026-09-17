// Two choices that compose: where the bytes come from, and how many cores
// hash them. The same file, hashed both ways, gives the same digest.

#include <blake3pp/blake3pp.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

// Larger than the default 8 MiB window, so the parallel path really has
// more than one window to fan out.
constexpr std::size_t demo_bytes = 12u << 20;

std::filesystem::path write_demo_file(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / "corpus.bin";
  std::ofstream out(path, std::ios::binary);
  std::vector<char> block(1 << 20);
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = static_cast<char>(i * 31 + 7);
  }
  for (std::size_t written = 0; written < demo_bytes; written += block.size()) {
    out.write(block.data(), static_cast<std::streamsize>(block.size()));
  }
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path path;
  std::filesystem::path scratch;
  if (argc > 1) {
    path = argv[1];
  } else {
    scratch = std::filesystem::temp_directory_path() / "blake3pp-example-09";
    std::filesystem::create_directories(scratch);
    path = write_demo_file(scratch);
  }
  std::cout << path.string() << ", "
            << std::filesystem::file_size(path) / (1024 * 1024) << " MiB\n\n";

  // One core. The reads are still asynchronous and still bypass the page
  // cache; what is sequential is the hashing.
  const blake3pp::digest one = blake3pp::hash_file(path);
  std::cout << "one core\n  " << one.to_hex() << '\n';

  // Every core. The only change to the call is the scheduler.
  auto sched = blake3pp::get_parallel_scheduler();
  const blake3pp::digest many = blake3pp::hash_file(path, sched);
  std::cout << "\nevery core\n  " << many.to_hex() << '\n';

  // The window size and the number of reads in flight are where the
  // pipeline is tuned to a device. Neither can change the answer.
  const blake3pp::digest tuned =
      blake3pp::hash_file(path, sched, {.window_bytes = 4 * 1024 * 1024,
                                        .queue_depth = 8});
  std::cout << "\nevery core, 4 MiB windows, 8 in flight\n  " << tuned.to_hex()
            << '\n';

  std::cout << "\nall three agree: " << std::boolalpha
            << (one == many && many == tuned) << '\n';

  if (!scratch.empty()) {
    std::filesystem::remove_all(scratch);
  }
}
