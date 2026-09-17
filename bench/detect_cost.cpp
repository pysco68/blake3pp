// What the detection ladder costs. The first library call in a fresh
// process builds the availability table, reading auxv, hwprobe or CPUID
// once per compiled variant. Later calls read the table.
//
// This prints those times for the process it runs in. The bundle's
// dispatch-cost.sh runs it several times, since only a new process pays
// the first call.
#include <blake3pp/core.hpp>
#include <blake3pp/dispatch.hpp>

#include <chrono>
#include <cstdio>
#include <ratio>

int main() {
  using clock = std::chrono::steady_clock;
  const auto us = [](clock::time_point a, clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
  };

  const auto t0 = clock::now();
  const blake3pp::arch best = blake3pp::best_available();   // builds the table
  const auto t1 = clock::now();
  const blake3pp::arch again = blake3pp::best_available();  // reads it
  const auto t2 = clock::now();
  const blake3pp::hasher h;                                  // auto_detect resolved
  const auto t3 = clock::now();
  const bool probed = blake3pp::run_trap_probes();           // the opt-in rung; a no-op off RISC-V
  const auto t4 = clock::now();

  std::printf("best_available(), first call:  %8.2f us  -> %s\n", us(t0, t1), blake3pp::to_string(best));
  std::printf("best_available(), second call: %8.3f us  -> %s\n", us(t1, t2), blake3pp::to_string(again));
  std::printf("hasher{} (auto_detect):        %8.3f us  -> %s\n", us(t2, t3), blake3pp::to_string(h.selected_arch()));
  std::printf("run_trap_probes():             %8.2f us  -> %s\n", us(t3, t4), probed ? "learned something" : "nothing to learn");
  for (const blake3pp::arch a : blake3pp::compiled_arches()) {
    const auto s = clock::now();
    const bool ok = blake3pp::is_available(a);
    const auto e = clock::now();
    std::printf("is_available(%-9s):        %8.3f us  -> %s\n", blake3pp::to_string(a), us(s, e), ok ? "yes" : "no");
  }
  return 0;
}
