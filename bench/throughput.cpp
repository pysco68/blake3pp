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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
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
// the same seam every portable variant uses, so each reference row differs
// from its same-ISA blake3pp twin ONLY in the hash_many kernel. Only the
// flag argument widths differ (the reference narrows them to uint8_t). The
// comparison rows come from this: the portable C kernel (the scalar row's
// direct counterpart) and every hand-tuned wide kernel: hand-scheduled
// AVX2 and AVX-512 assembly on x86-64 (GAS-built under gcc/clang/clang-cl,
// MASM under MSVC), NEON intrinsics on aarch64 (the reference ships no ARM
// assembly).
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

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
#if defined(__x86_64__) || defined(_M_X64)
extern "C" void blake3_hash_many_avx2(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint8_t flags, std::uint8_t flags_start,
    std::uint8_t flags_end, std::uint8_t* out);
extern "C" void blake3_hash_many_avx512(
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
// One forwarding wrapper per reference kernel; the only work is widening
// the flag arguments back to the blake3pp signature.
template <auto ReferenceFn>
void asm_hash_many(const std::uint8_t* const* inputs, std::size_t num_inputs,
                   std::size_t blocks, const std::uint32_t key[8],
                   std::uint64_t counter, bool increment_counter,
                   std::uint32_t flags, std::uint32_t flags_start,
                   std::uint32_t flags_end, std::uint8_t* out) noexcept {
  ReferenceFn(inputs, num_inputs, blocks, key, counter, increment_counter,
              static_cast<std::uint8_t>(flags),
              static_cast<std::uint8_t>(flags_start),
              static_cast<std::uint8_t>(flags_end), out);
}

// Everything that varies by host architecture, decided once: which wide
// kernels exist, which of our variants each pairs with, and how the rows
// read. Narrowest first, so the widest AVAILABLE entry (the ISA the
// reference's own dispatcher would pick) is the last match, and the one
// the parallel row below reuses.
struct asm_kernel {
  blake3pp::arch variant;
  const char* row;
  const char* note;
  decltype(blake3pp::kern::kernel_ops::hash_many) hash_many;
};

#if defined(__x86_64__) || defined(_M_X64)
constexpr asm_kernel asm_kernels[] = {
    {blake3pp::arch::avx2, "avx2", "hand-written asm",
     &asm_hash_many<&blake3_hash_many_avx2>},
    {blake3pp::arch::avx512, "avx512", "hand-written asm",
     &asm_hash_many<&blake3_hash_many_avx512>},
};
#else
constexpr asm_kernel asm_kernels[] = {
    {blake3pp::arch::neon, "neon", "hand-tuned intrinsics",
     &asm_hash_many<&blake3_hash_many_neon>},
};
#endif
}  // namespace
#endif
#endif

namespace {
using b3tool::println;

// Why the t16 tuner can disagree with the table above, settled by
// measurement rather than argument. tune_transpose16() races the three
// strategies over a 16 KiB working set reused hundreds of times (L1
// resident throughout), while the benchmark rows stream 512 MiB out of
// DRAM. If the strategies rank differently in those two regimes, the tuner
// is calibrated for the wrong one.
//
// This sweeps the SAME comparison across working-set sizes, and runs each
// size in both strategy orders: a ranking that flips with size is a regime
// mismatch, a ranking that flips with ORDER is thermal/frequency drift
// contaminating the measurement instead.
// The width-16 kernel this machine would dispatch to (avx512 on x86,
// sve512/sve2_512 elsewhere), if any: the t16 dial rows and the sweep race
// whichever it is.
std::optional<blake3pp::arch> first_w16_arch() {
  for (const auto a : blake3pp::available_arches()) {
    if (blake3pp::detail::resolve(a)->simd_degree == 16) {
      return a;
    }
  }
  return std::nullopt;
}

void t16_sweep(int reps, double cooldown_s) {
  const auto w16 = first_w16_arch();
  if (!w16.has_value()) {
    println(stdout, "t16 sweep needs a width-16 kernel; none available here");
    return;
  }
  constexpr std::size_t work = 128u << 20;  // hashed bytes per timed rep
  const std::size_t sizes[] = {16u << 10,  128u << 10, 1u << 20,
                               8u << 20,   64u << 20,  512u << 20};
  const blake3pp::transpose16 order[] = {blake3pp::transpose16::staging,
                                         blake3pp::transpose16::tree,
                                         blake3pp::transpose16::quartered};

  std::vector<std::byte> buf(sizes[std::size(sizes) - 1]);
  for (std::size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<std::byte>(i % 251);
  }
  const auto saved = blake3pp::active_transpose16();
  b3tool::cooldown cooldown(cooldown_s);

  println(stdout,
          "t16 strategy vs working-set size ({}, {} MiB hashed per rep,\n"
          "best of {}; 'rev' repeats the size with the strategy order "
          "reversed)\n",
          blake3pp::to_string(*w16), work >> 20, reps);
  println(stdout, "  {:>10}  {:>10} {:>10} {:>10}   winner", "working set",
          "staging", "tree", "quartered");

  for (const std::size_t size : sizes) {
    for (const bool reverse : {false, true}) {
      const std::size_t iters = std::max<std::size_t>(1, work / size);
      double best[3] = {0, 0, 0};
      for (int k = 0; k < 3; ++k) {
        const int idx = reverse ? 2 - k : k;
        cooldown();
        blake3pp::set_transpose16(order[idx]);
        const double s = b3tool::best_seconds(reps, /*warmup=*/true, [&] {
          for (std::size_t it = 0; it < iters; ++it) {
            blake3pp::hasher h{*w16};
            h.update(std::span<const std::byte>{buf.data(), size});
            (void)h.finalize();
          }
        });
        best[idx] = b3tool::gib_per_s(size * iters, s);
      }
      const int win = static_cast<int>(
          std::max_element(best, best + 3) - best);
      const std::string label =
          size >= (1u << 20)
              ? std::format("{} MiB", size >> 20)
              : std::format("{} KiB", size >> 10);
      println(stdout, "  {:>10}  {:10.2f} {:10.2f} {:10.2f}   {}{}", label,
              best[0], best[1], best[2],
              blake3pp::to_string(order[win]), reverse ? "  (rev)" : "");
    }
  }
  blake3pp::set_transpose16(saved);
  cooldown();
  println(stdout, "\n  tune_transpose16() picks: {}   (its own race uses a "
                  "16 KiB working set)",
          blake3pp::to_string(blake3pp::tune_transpose16()));
  blake3pp::set_transpose16(saved);
}
}  // namespace

int main(int argc, char** argv) {
  std::size_t mib = 512;
  int reps = 5;
  double cooldown_s = 5.0;
  unsigned pool_threads = 0;
  bool sweep_t16 = false;
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
  app.add_option("--threads", pool_threads,
                 "parallel-engine threads (0 = hardware concurrency)");
  app.add_flag("--t16-sweep", sweep_t16,
               "diagnose the AVX-512 transpose tuner: rank the three "
               "strategies across working-set sizes and strategy orders");

  // The name<->enum mapping comes from the library's canonical list; the
  // bench never re-enumerates the arch enum.
  std::map<std::string, blake3pp::arch> arch_names;
  for (const auto a : blake3pp::all_arches()) {
    arch_names.emplace(blake3pp::to_string(a), a);
  }
  app.add_option("arch", arches, "SIMD variants to measure")
      ->transform(CLI::CheckedTransformer(arch_names, CLI::ignore_case));
  CLI11_PARSE(app, argc, argv);

  if (sweep_t16) {
    t16_sweep(reps, cooldown_s);
    return 0;
  }

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

  // The width-16 transpose strategies, raced in-process (the dial is
  // runtime; no per-strategy binaries needed), plus what the tuner picks.
  if (const auto w16 = first_w16_arch(); w16.has_value()) {
    const auto saved = blake3pp::active_transpose16();
    for (const auto strat :
         {blake3pp::transpose16::staging, blake3pp::transpose16::tree,
          blake3pp::transpose16::quartered}) {
      blake3pp::set_transpose16(strat);
      row(std::format("  t16-{}", blake3pp::to_string(strat)), "",
          [&] { return hash_with(blake3pp::detail::resolve(*w16)); },
          14);
    }
    blake3pp::set_transpose16(saved);
    cooldown();
    // Tuned for THIS run's input size, so the verdict is comparable to the
    // three rows above it. The winner depends on the size (cache-resident
    // inputs rank the strategies differently from streaming ones), so a
    // tuner asked about a different size than the table measures has every
    // right to disagree with it.
    println(stdout, "    t16 tuner picks: {} (tuned for {} MiB)",
            blake3pp::to_string(blake3pp::tune_transpose16(input.size())),
            mib);
    blake3pp::set_transpose16(saved);
  }

#if defined(BLAKE3PP_BENCH_UPSTREAM)
  // The reference kernels behind the blake3pp kernel_ops seam: identical
  // pipeline, only hash_many swapped, so any difference to the section
  // above is pure kernel codegen. Row labels match the blake3pp rows they
  // pair with (portable <-> scalar, neon <-> neon, avx2 <-> avx2, avx512
  // <-> avx512); always compare a reference row against its same-ISA twin,
  // never against the end-to-end `reference` row, which picks its own
  // best ISA.
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

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
  // One row per wide reference kernel this machine can run. The widest one
  // is kept for the parallel row further down, the same ISA the reference
  // dispatcher would have chosen for itself.
  blake3pp::kern::kernel_ops asm_ops{};
  const char* asm_row = nullptr;
  for (const auto& k : asm_kernels) {
    if (!blake3pp::is_available(k.variant)) {
      continue;
    }
    blake3pp::kern::kernel_ops ops = *blake3pp::detail::resolve(k.variant);
    ops.hash_many = k.hash_many;  // compress_in_place stays portable
    row(k.row, k.note, [&] { return hash_with(&ops); });
    asm_ops = ops;
    asm_row = k.row;
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

  // Our own full pipeline, single thread: the discriminator between
  // "the pool scales badly" and "our pipeline (subtree/CV machinery)
  // costs more than the raw kernel loop at this width". The reference
  // e2e row above has had this mirror all along; ours was the blind
  // spot while chasing the width-16 pool anomaly.
  println(stdout, "\nblake3pp end-to-end (single thread)");
  row("blake3pp", "", [&] {
    return blake3pp::hash(std::span<const std::byte>{input});
  });

  // The sender-based parallel engine over the whole tree: the numbers that
  // matter for feeding modern storage. Rows are kernel/scheduler pairings.
  // --threads is honored exactly under stdexec (an owned pool of that
  // size); other providers fall back to the process-wide scheduler.
  const unsigned nthreads =
      pool_threads != 0 ? pool_threads : std::thread::hardware_concurrency();
  println(stdout, "\nparallel engine ({} threads)", nthreads);
  // An OWNED pool at exactly nthreads, not b3tool::compute_pool, whose
  // scheduler() has a threads>1 precondition (--threads 1 is a valid and
  // interesting measurement here: pool machinery at zero parallelism).
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
  exec::static_thread_pool engine_pool{nthreads};
#endif
  auto engine_sched =
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
      engine_pool.get_scheduler();
#else
      blake3pp::get_parallel_scheduler();
#endif
  {
    auto sched = engine_sched;
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
    (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__))
  // The widest reference kernel on the same parallel engine: separates
  // kernel from scheduler in the rows above.
  if (asm_row != nullptr) {
    auto sched = engine_sched;
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
