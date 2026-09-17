// Spreading one hash over every core, and why the answer does not change.

#include <blake3pp/blake3pp.hpp>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

int main() {
  // 256 MiB of something to hash.
  std::vector<std::byte> buffer(256u << 20);
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = static_cast<std::byte>(i);
  }

  const auto time = [&](auto&& fn) {
    const auto start = std::chrono::steady_clock::now();
    const blake3pp::digest d = fn();
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - start;
    const double gibps =
        static_cast<double>(buffer.size()) / elapsed.count() / (1u << 30);
    return std::pair{d, gibps};
  };

  const auto [sequential, seq_rate] =
      time([&] { return blake3pp::hash(buffer); });
  std::cout << "one core\n  " << sequential.to_hex() << "\n  " << seq_rate
            << " GiB/s\n";

  // Adding cores means adding a scheduler. get_parallel_scheduler() is
  // the process-wide one in P2079's shape, which is the same call C++26
  // application code makes.
  auto sched = blake3pp::get_parallel_scheduler();

  const auto [parallel, par_rate] =
      time([&] { return blake3pp::hash(buffer, sched); });
  std::cout << "\nevery core\n  " << parallel.to_hex() << "\n  " << par_rate
            << " GiB/s\n";

  // BLAKE3's tree makes the decomposition exact rather than approximate,
  // so the parallel digest is the sequential digest. Splitting the work
  // is not an approximation of anything.
  std::cout << "\nsame digest: " << std::boolalpha
            << (sequential == parallel) << '\n';
  std::cout << "speedup: " << (par_rate / seq_rate) << " x\n";

  // The incremental counterpart, for input that arrives in pieces.
  blake3pp::parallel_hasher ph{sched};
  for (int i = 0; i < 4; ++i) {
    ph.update(buffer);
  }
  std::cout << "\n1 GiB through parallel_hasher\n  " << ph.finalize().to_hex()
            << '\n';

  // Each part's 32-byte chaining value waits on the calling thread's
  // stack, and stack_budget caps how much stack that table may take. The
  // default is 32 KiB, which is 1024 parts. A smaller budget splits the
  // input into fewer, larger parts, which is what a small thread wants.
  const blake3pp::digest thrifty =
      blake3pp::hash<blake3pp::stack_budget{1024}>(buffer, sched);
  std::cout << "\nwith a 1 KiB part table (32 parts)\n  " << thrifty.to_hex()
            << '\n';
  std::cout << "\nstill the same digest: " << (thrifty == sequential) << '\n';
}
