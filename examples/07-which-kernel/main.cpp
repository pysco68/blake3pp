// What this binary carries, what this CPU can run, and how to override
// the choice.

#include <blake3pp/blake3pp.hpp>

#include <cstddef>
#include <iostream>
#include <vector>

int main() {
  // The build configuration, which is what belongs in a diagnostics
  // banner or a bug report.
  std::cout << "blake3pp " << blake3pp::version() << '\n'
            << "  simd provider      " << blake3pp::simd_provider() << '\n'
            << "  execution provider " << blake3pp::execution_provider()
            << "\n\n";

  // Every variant the target platform supports is compiled into the
  // binary. This is a property of the build.
  std::cout << "compiled into this binary:";
  for (const blake3pp::arch a : blake3pp::compiled_arches()) {
    std::cout << ' ' << blake3pp::to_string(a);
  }
  std::cout << '\n';

  // Which of them will actually run is a property of the CPU, decided at
  // run time and ordered best first.
  std::cout << "usable on this CPU:      ";
  for (const blake3pp::arch a : blake3pp::available_arches()) {
    std::cout << ' ' << blake3pp::to_string(a);
  }
  std::cout << '\n';

  std::cout << "dispatch picks:           "
            << blake3pp::to_string(blake3pp::best_available()) << '\n';
  std::cout << "\nthe best available one is the first listed: " << std::boolalpha
            << (blake3pp::available_arches().front() ==
                blake3pp::best_available())
            << '\n';

  // A hasher takes a variant if you want to choose for yourself, which is
  // how the tests and the benchmarks compare kernels against each other.
  // Every variant produces the same digest.
  const std::vector<std::byte> data(1u << 20, std::byte{0x5a});
  const blake3pp::digest reference = blake3pp::hash(data);

  std::cout << "\neach variant on the same 1 MiB:\n";
  for (const blake3pp::arch a : blake3pp::available_arches()) {
    const blake3pp::hasher pinned{a};
    blake3pp::hasher h = pinned;
    h.update(data);
    std::cout << "  " << blake3pp::to_string(a) << ": "
              << (h.finalize() == reference ? "same digest" : "DIFFERENT")
              << '\n';
  }

  // Asking for a variant the CPU cannot run is not an error: dispatch
  // falls back to the best one available. is_available() answers the
  // question beforehand.
  std::cout << "\nis scalar available? "
            << blake3pp::is_available(blake3pp::arch::scalar) << '\n';
}
