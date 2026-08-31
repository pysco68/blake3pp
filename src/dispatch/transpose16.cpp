// The width-16 transpose dial and its stopwatch tuner: runtime TUNING
// policy, deliberately separate from dispatch (which only ever answers
// "can this CPU run that kernel"; this file answers "which of three
// correct strategies is fastest HERE", a question no feature bit can).

#include <blake3pp/dispatch.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "kernel/kernel.hpp"

namespace blake3pp {

namespace kern {
// Definition of the dial declared in kernel.hpp; every kernel TU reads it
// with a relaxed load. Quartered is the measured-best default.
std::atomic<transpose16_mode> transpose16_active{
    transpose16_mode::quartered};
}  // namespace kern

void set_transpose16(transpose16 strategy) noexcept {
  kern::transpose16_active.store(
      static_cast<kern::transpose16_mode>(strategy),
      std::memory_order_relaxed);
}

transpose16 active_transpose16() noexcept {
  return static_cast<transpose16>(
      kern::transpose16_active.load(std::memory_order_relaxed));
}

std::optional<transpose16> transpose16_from_string(
    std::string_view name) noexcept {
  for (const transpose16 t : {transpose16::staging, transpose16::tree,
                              transpose16::quartered}) {
    if (name == to_string(t)) {
      return t;
    }
  }
  return std::nullopt;
}

std::string_view to_string(transpose16 strategy) noexcept {
  switch (strategy) {
    case transpose16::staging:   return "staging";
    case transpose16::tree:      return "tree";
    case transpose16::quartered: return "quartered";
  }
  return "unknown";
}

// Races the three strategies on this CPU and applies the winner. No CPUID
// bit distinguishes a double-pumped from a full-width AVX-512 datapath
// (Strix Point and Granite Ridge report identical feature flags), so the
// only honest detector is a stopwatch.
//
// The race walks a buffer sized like the caller's inputs, ONE pass per
// timed repetition, because the winner depends on where the data lives as
// much as on the CPU. Measured on a Ryzen AI Max 395: quartered wins by
// 18% over staging when the input fits in last-level cache (8 MiB) and
// LOSES to it by 8% when the input streams from DRAM (512 MiB), both
// reproducible. An earlier version of this race hashed the same 16 KiB
// ~800 times (L1-resident throughout) and duly picked the cache-resident
// winner for every caller, including ones hashing gigabyte files, where it
// selected the slowest of the three.
//
// This is a heuristic, not an oracle: it samples one size on one machine
// while it is not doing the caller's real work. Callers who need the last
// few percent should measure with blake3pp_bench and pin the result with
// set_transpose16().
transpose16 tune_transpose16(std::size_t typical_input_bytes) noexcept {
  // Race whichever width-16 kernel this machine would actually dispatch to
  // (avx512, sve512/sve2_512, rvv512...): available_arches() is best-first,
  // so the first width-16 entry is the one auto_detect would pick.
  const kern::kernel_ops* ops = nullptr;
  for (const arch a : available_arches()) {
    const kern::kernel_ops* k = detail::resolve(a);
    if (k->simd_degree == 16) {
      ops = k;
      break;
    }
  }
  if (ops == nullptr) {
    return active_transpose16();  // dial is inert without a W=16 kernel
  }
  static constexpr std::size_t chunk = 1024;  // BLAKE3 chunk
  static constexpr std::size_t lanes = 16;    // the W=16 kernel's batch
  static constexpr std::size_t batch = lanes * chunk;   // 16 KiB per step
  static constexpr std::size_t cap = 256u << 20;

  // Whole batches, at least one, and never more than the cap: a caller
  // hashing 40 GiB files does not get a 40 GiB race.
  std::size_t bytes = typical_input_bytes > cap ? cap : typical_input_bytes;
  bytes = (bytes / batch) * batch;
  if (bytes == 0) {
    bytes = batch;
  }

  // Heap, not stack: the point is a working set that does not fit in
  // cache. Tuning must not fail the program, so an allocation failure
  // simply leaves the current setting alone.
  auto* data = static_cast<std::uint8_t*>(std::malloc(bytes));
  if (data == nullptr) {
    return active_transpose16();
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    data[i] = static_cast<std::uint8_t>(i % 251);
  }
  constexpr std::uint32_t key[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u,
                                    0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu,
                                    0x1F83D9ABu, 0x5BE0CD19u};
  std::array<std::uint8_t, lanes * 32> out;

  const transpose16 saved = active_transpose16();
  const std::size_t batches = bytes / batch;
  const auto one_pass = [&] {
    for (std::size_t b = 0; b < batches; ++b) {
      const std::uint8_t* inputs[lanes];
      for (std::size_t i = 0; i < lanes; ++i) {
        inputs[i] = data + b * batch + i * chunk;
      }
      ops->hash_many(inputs, lanes, chunk / 64, key, 0, true, 0,
                     kern::flag_chunk_start, kern::flag_chunk_end,
                     out.data());
    }
  };
  // INTERLEAVED repetitions, not per-strategy blocks: under drifting
  // conditions (laptop boost droop, a power-clamped OS profile, noisy
  // cloud neighbors) a sequential race hands later strategies a worse
  // environment; a power-clamped Windows box measurably picked the
  // second-raced strategy over a 37%-faster one that raced last. Round-
  // robin spreads the drift evenly; best-of per strategy still filters
  // one-off stalls. (The bench's --t16-sweep guards against the same
  // effect with its order-reversed 'rev' rows.)
  constexpr transpose16 modes[] = {transpose16::staging, transpose16::tree,
                                   transpose16::quartered};
  std::chrono::steady_clock::duration best[3] = {
      std::chrono::steady_clock::duration::max(),
      std::chrono::steady_clock::duration::max(),
      std::chrono::steady_clock::duration::max()};
  for (const transpose16 mode : modes) {
    set_transpose16(mode);
    one_pass();  // warm-up: frequency ramp, page faults, TLB, per mode
  }
  for (int rep = 0; rep < 3; ++rep) {
    for (std::size_t i = 0; i < 3; ++i) {
      set_transpose16(modes[i]);
      const auto t0 = std::chrono::steady_clock::now();
      one_pass();
      const auto dt = std::chrono::steady_clock::now() - t0;
      if (dt < best[i]) {
        best[i] = dt;
      }
    }
  }
  transpose16 winner = saved;
  auto winner_time = std::chrono::steady_clock::duration::max();
  for (std::size_t i = 0; i < 3; ++i) {
    if (best[i] < winner_time) {
      winner_time = best[i];
      winner = modes[i];
    }
  }
  std::free(data);
  set_transpose16(winner);
  return winner;
}

transpose16 tune_transpose16() noexcept {
  return tune_transpose16(default_tune_bytes);
}

}  // namespace blake3pp
