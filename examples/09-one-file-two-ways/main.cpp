// Two choices that compose: where the bytes come from, and how many cores
// read them. The same file, hashed both ways, gives the same digest.

#include <blake3pp/blake3pp.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

std::filesystem::path write_demo_file(const std::filesystem::path& dir,
                                      std::size_t bytes) {
  const std::filesystem::path path = dir / "corpus.bin";
  std::ofstream out(path, std::ios::binary);
  std::vector<char> block(1 << 20);
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = static_cast<char>(i * 31 + 7);
  }
  for (std::size_t written = 0; written < bytes; written += block.size()) {
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
    path = write_demo_file(scratch, 12u << 20);
  }
  const auto bytes = std::filesystem::file_size(path);

  // One untimed pass first. The first read of a freshly written file pays
  // for metadata and for opening the direct-I/O path, which at these sizes
  // is larger than the difference being measured.
  (void)blake3pp::hash_file(path);

  const auto timed = [&](auto&& run) {
    const auto start = std::chrono::steady_clock::now();
    const blake3pp::digest d = run();
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - start;
    return std::pair{d, static_cast<double>(bytes) / elapsed.count() / (1u << 30)};
  };

  // One core. The reads are still asynchronous and still bypass the page
  // cache; what is sequential is the hashing.
  const auto [one, one_rate] = timed([&] { return blake3pp::hash_file(path); });
  std::cout << "one core\n  " << one.to_hex() << "\n  " << one_rate
            << " GiB/s\n";

  // Every core. The only change to the call is the scheduler.
  auto sched = blake3pp::get_parallel_scheduler();
  const auto [many, many_rate] =
      timed([&] { return blake3pp::hash_file(path, sched); });
  std::cout << "\nevery core\n  " << many.to_hex() << "\n  " << many_rate
            << " GiB/s\n";

  std::cout << "\nsame digest: " << std::boolalpha << (one == many) << '\n';
  std::cout << "speedup: " << (many_rate / one_rate) << " x\n";

  // Past a point the device is the limit rather than the hashing, and the
  // window size and queue depth are what move it. The window has to divide
  // the file into more than one piece for any of this to matter, which is
  // why a small file wants a small window.
  const auto [tuned, tuned_rate] = timed([&] {
    return blake3pp::hash_file(path, sched,
                               {.window_bytes = 4 * 1024 * 1024,
                                .queue_depth = 8});
  });
  std::cout << "\nwith a 4 MiB window, 8 deep\n  " << tuned_rate
            << " GiB/s, digest unchanged: " << (tuned == one) << '\n';

  if (!scratch.empty()) {
    std::filesystem::remove_all(scratch);
  }
}
