// Hashing throughput, grouped into comparable sections: blake3pp's own
// kernels, the reference implementation's kernels driven by the blake3pp
// pipeline, the reference implementation end-to-end, and the parallel
// engine under different schedulers/kernels.
//
//   blake3pp_bench [--size MiB] [--reps N] [--cooldown S] [ARCH...]
//
// With no ARCH arguments, measures every variant available on this machine.
// Reports the best of N repetitions, the interesting number for a
// throughput ceiling; interference only ever slows a run down. Deliberately
// framework-free: pair it with hyperfine when process-level statistics are
// wanted.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <blake3pp/blake3pp.hpp>
#include <blake3pp/parallel.hpp>

#include "tool_common.hpp"

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
#include <exec/libdispatch_queue.hpp>
#endif

#if defined(BLAKE3PP_BENCH_UPSTREAM)
#include <blake3.h>

#include "kernel/kernel.hpp"

// The reference implementation's kernels, already present in the linked
// baseline library, wrapped as kernel_ops tables: that plugs them into the
// blake3pp pipeline (subtree batching, CV stack, parallel engine) through
// the same seam every portable variant uses. Only the flag argument widths
// differ (the reference narrows them to uint8_t). Two comparison rows come
// from this: the portable C kernel (the scalar row's direct counterpart)
// and the hand-tuned wide kernel: hand-scheduled AVX2 assembly on x86-64,
// NEON intrinsics on aarch64 (the reference ships no ARM assembly).
extern "C" void blake3_hash_many_portable(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint8_t flags, std::uint8_t flags_start,
    std::uint8_t flags_end, std::uint8_t* out);

namespace {
void portable_hash_many(const std::uint8_t* const* inputs,
                        std::size_t num_inputs, std::size_t blocks,
                        const std::uint32_t key[8], std::uint64_t counter,
                        bool increment_counter, std::uint32_t flags,
                        std::uint32_t flags_start, std::uint32_t flags_end,
                        std::uint8_t* out) noexcept {
  blake3_hash_many_portable(inputs, num_inputs, blocks, key, counter,
                            increment_counter,
                            static_cast<std::uint8_t>(flags),
                            static_cast<std::uint8_t>(flags_start),
                            static_cast<std::uint8_t>(flags_end), out);
}
}  // namespace

#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
extern "C" void blake3_hash_many_avx2(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint8_t flags, std::uint8_t flags_start,
    std::uint8_t flags_end, std::uint8_t* out);
#else
extern "C" void blake3_hash_many_neon(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint8_t flags, std::uint8_t flags_start,
    std::uint8_t flags_end, std::uint8_t* out);
#endif

namespace {
// Everything that varies by host architecture, decided once: which kernel
// exists, which of our variants it pairs with, and how the row reads.
#if defined(__x86_64__)
constexpr auto upstream_hash_many = &blake3_hash_many_avx2;
constexpr auto asm_arch = blake3pp::arch::avx2;
constexpr const char* asm_row = "avx2";
constexpr const char* asm_note = "hand-written asm";
#else
constexpr auto upstream_hash_many = &blake3_hash_many_neon;
constexpr auto asm_arch = blake3pp::arch::neon;
constexpr const char* asm_row = "neon";
constexpr const char* asm_note = "hand-tuned intrinsics";
#endif

void asm_hash_many(const std::uint8_t* const* inputs, std::size_t num_inputs,
                   std::size_t blocks, const std::uint32_t key[8],
                   std::uint64_t counter, bool increment_counter,
                   std::uint32_t flags, std::uint32_t flags_start,
                   std::uint32_t flags_end, std::uint8_t* out) noexcept {
  upstream_hash_many(inputs, num_inputs, blocks, key, counter,
                     increment_counter, static_cast<std::uint8_t>(flags),
                     static_cast<std::uint8_t>(flags_start),
                     static_cast<std::uint8_t>(flags_end), out);
}
}  // namespace
#endif
#endif

namespace {
using b3tool::println;
}  // namespace

int main(int argc, char** argv) {
  std::size_t mib = 512;
  int reps = 5;
  double cooldown_s = 5.0;
  std::vector<blake3pp::arch> arches;

  CLI::App app{
      "Single-thread and parallel BLAKE3 hashing throughput.\n"
      "With no ARCH, measures every variant available on this machine."};
  app.add_option("--size", mib, "input size in MiB")
      ->check(CLI::PositiveNumber)
      ->capture_default_str();
  app.add_option("--reps", reps, "timed repetitions (best wins)")
      ->check(CLI::PositiveNumber)
      ->capture_default_str();
  app.add_option("--cooldown", cooldown_s,
                 "idle seconds between measurements (0 disables)")
      ->capture_default_str();

  // The name<->enum mapping comes from the library's canonical list; the
  // bench never re-enumerates the arch enum.
  std::map<std::string, blake3pp::arch> arch_names;
  for (const auto a : blake3pp::all_arches()) {
    arch_names.emplace(blake3pp::to_string(a), a);
  }
  app.add_option("arch", arches, "SIMD variants to measure")
      ->transform(CLI::CheckedTransformer(arch_names, CLI::ignore_case));
  CLI11_PARSE(app, argc, argv);

  if (arches.empty()) {
    // Available variants, printed worst-to-best so the table reads as an
    // ascending progression.
    const auto avail = blake3pp::available_arches();
    arches.assign(avail.rbegin(), avail.rend());
  }

  std::vector<std::byte> input(mib * 1024 * 1024);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::byte>(i % 251);
  }

  println(stdout, "blake3pp throughput, {} MiB, best of {}, {:.0f}s cooldown",
          mib, reps, cooldown_s);
  println(stdout, "auto resolves to: {}\n",
          blake3pp::to_string(blake3pp::best_available()));

  b3tool::cooldown cooldown(cooldown_s);

  // One measured table row: cool down, time the best of `reps` (plus an
  // uncounted warmup), print. Every row in every section goes through here.
  const auto row = [&](std::string_view label, std::string_view note,
                       auto&& fn, int width = 10) {
    cooldown();
    blake3pp::digest d{};
    const double best =
        b3tool::best_seconds(reps, /*warmup=*/true, [&] { d = fn(); });
    const double gib = b3tool::gib_per_s(input.size(), best);
    if (note.empty()) {
      println(stdout, "  {:<{}} {:8.2f} GiB/s   ({}...)", label, width, gib,
              d.to_hex().substr(0, 16));
    } else {
      println(stdout, "  {:<{}} {:8.2f} GiB/s   ({}...)  [{}]", label, width,
              gib, d.to_hex().substr(0, 16), note);
    }
  };

  // Hashes the whole input with one kernel table, sequentially.
  const auto hash_with = [&](const blake3pp::kern::kernel_ops* ops) {
    blake3pp::hasher h{ops};
    h.update(std::span<const std::byte>{input});
    return h.finalize();
  };

  println(stdout, "blake3pp kernels (single thread)");
  for (const auto a : arches) {
    if (!blake3pp::is_available(a)) {
      println(stdout, "  {:<10} unavailable on this machine",
              blake3pp::to_string(a));
      continue;
    }
    row(blake3pp::to_string(a), "",
        [&] { return hash_with(blake3pp::detail::resolve(a)); });
  }

  // The AVX-512 transpose strategies, raced in-process (the dial is
  // runtime; no per-strategy binaries needed), plus what the tuner picks.
  if (blake3pp::is_available(blake3pp::arch::avx512)) {
    const auto saved = blake3pp::active_transpose16();
    for (const auto strat :
         {blake3pp::transpose16::staging, blake3pp::transpose16::tree,
          blake3pp::transpose16::quartered}) {
      blake3pp::set_transpose16(strat);
      row(std::format("  t16-{}", blake3pp::to_string(strat)), "",
          [&] { return hash_with(
                    blake3pp::detail::resolve(blake3pp::arch::avx512)); },
          14);
    }
    blake3pp::set_transpose16(saved);
    cooldown();
    println(stdout, "    t16 tuner picks: {}",
            blake3pp::to_string(blake3pp::tune_transpose16()));
  }

#if defined(BLAKE3PP_BENCH_UPSTREAM)
  // The reference kernels behind the blake3pp kernel_ops seam: identical
  // pipeline, only hash_many swapped, so any difference to the section above
  // is pure kernel codegen. Row labels match the blake3pp rows they pair
  // with (portable <-> scalar, neon <-> neon, avx2 <-> avx2).
  println(stdout,
          "\nreference impl. kernels in the blake3pp pipeline "
          "(single thread, BLAKE3 {})",
          BLAKE3_VERSION_STRING);
  {
    blake3pp::kern::kernel_ops port_ops =
        *blake3pp::detail::resolve(blake3pp::arch::scalar);
    port_ops.hash_many = &portable_hash_many;
    row("portable", "", [&] { return hash_with(&port_ops); });
  }

#if defined(__x86_64__) || defined(__aarch64__)
  blake3pp::kern::kernel_ops asm_ops{};
  const bool have_asm = blake3pp::is_available(asm_arch);
  if (have_asm) {
    asm_ops = *blake3pp::detail::resolve(asm_arch);
    asm_ops.hash_many = &asm_hash_many;  // compress_in_place stays portable
    row(asm_row, asm_note, [&] { return hash_with(&asm_ops); });
  }
#endif

  // The reference implementation as shipped: its own runtime dispatch, same
  // input. The digest must match every row above.
  println(stdout, "\nreference impl. end-to-end (single thread, own dispatch)");
  row("reference", "", [&] {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, input.data(), input.size());
    blake3pp::digest d{};
    blake3_hasher_finalize(&h, reinterpret_cast<std::uint8_t*>(d.bytes.data()),
                           BLAKE3_OUT_LEN);
    return d;
  });
#endif

  // The sender-based parallel engine over the whole tree: the numbers that
  // matter for feeding modern storage. Rows are kernel/scheduler pairings.
  const unsigned nthreads = std::thread::hardware_concurrency();
  println(stdout, "\nparallel engine ({} threads)", nthreads);
  {
    auto sched = blake3pp::get_parallel_scheduler();
    row("blake3pp/pool",
        std::format("{} pool", blake3pp::execution_provider()),
        [&] { return blake3pp::hash(std::span<const std::byte>{input}, sched); },
        15);
  }

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
  // The identical sender pipeline on Apple's platform runtime: stdexec's
  // libdispatch scheduler submits the bulk work to GCD's global pool
  // (QoS-placed across P/E cores by the OS) instead of a process-owned
  // thread pool. Same engine, different scheduler argument; compare with
  // the blake3pp/pool row.
  {
    exec::libdispatch_queue queue;
    auto sched = queue.get_scheduler();
    row("blake3pp/gcd", "stdexec on GCD",
        [&] { return blake3pp::hash(std::span<const std::byte>{input}, sched); },
        15);
  }
#endif

#if defined(BLAKE3PP_BENCH_UPSTREAM) && \
    (defined(__x86_64__) || defined(__aarch64__))
  // The reference wide kernel on the same parallel engine: separates kernel
  // from scheduler in the rows above.
  if (have_asm) {
    auto sched = blake3pp::get_parallel_scheduler();
    row("reference/pool",
        std::format("{} kernel, {} pool", asm_row,
                    blake3pp::execution_provider()),
        [&] {
          return blake3pp::hash(std::span<const std::byte>{input}, sched,
                                &asm_ops);
        },
        15);
  }
#endif
  return 0;
}
