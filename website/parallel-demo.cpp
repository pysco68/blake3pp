// ---- paste below the amalgamated library, replacing its main() --------
// Compiler Explorer: x86-64 gcc 16.2, -std=c++26 -O2 -msse4.2,
// and the beman.execution library selected.
#include <chrono>
#include <print>
#include <thread>
#include <vector>

int main() {
  std::vector<std::byte> data(32u << 20, std::byte{0xa5});   // 32 MiB

  auto timed = [&](auto&& run) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto digest = run();
    const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
    return std::pair{digest, data.size() / dt.count() / (1024.0 * 1024 * 1024)};
  };

  const auto [one, one_gibs] = timed([&] { return blake3pp::hash(data); });
  const auto [all, all_gibs] = timed([&] {
    return blake3pp::hash(data, blake3pp::get_parallel_scheduler());
  });

  std::println("kernel      {}", blake3pp::to_string(blake3pp::hasher{}.selected_arch()));
  std::println("hardware    {} threads", std::thread::hardware_concurrency());
  std::println("one core    {:.2f} GiB/s", one_gibs);
  std::println("all cores   {:.2f} GiB/s   ({:.1f} x)", all_gibs, all_gibs / one_gibs);
  std::println("same digest {}", one == all);
  std::println("{}", all.to_hex());
}
