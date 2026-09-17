// Hashing throughput, grouped into comparable sections: blake3pp's own
// kernels, the reference implementation's kernels driven by the blake3pp
// pipeline, the reference implementation end-to-end, and the parallel
// engine under different schedulers/kernels.
//
//   blake3pp_bench [--size MiB] [--reps N] [--cooldown S] [--pin CPU] [ARCH...]
//
// With no ARCH arguments, measures every variant available on this machine.
// Reports the best of N repetitions, the interesting number for a
// throughput ceiling; interference only ever slows a run down. Deliberately
// framework-free: pair it with hyperfine when process-level statistics are
// wanted.
//
// The single-thread rows run INTERLEAVED instead of row-by-row: a pass is
// one rep of every row, with the start row rotated per pass. Each row also
// prints the CPUs it ran on.
//
// Both are scar tissue from one incident. On a heterogeneous phone, an
// Exynos 2600, the row-by-row order let EAS upmigrate the run onto the
// prime core over time. Whichever rows ran LAST won a core class, not a
// comparison, which produced a 21% phantom that looked exactly like a
// microarchitecture finding.
//
// Interleaving makes placement and thermal history symmetric across rows.
// --pin removes the variable entirely. The per-row CPU report is how a
// reader of someone else's output can tell which regime they are looking
// at.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include <CLI/CLI.hpp>
#include <blake3pp/blake3pp.hpp>
#include <blake3pp/parallel.hpp>
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
#include <blake3pp/parallel_backend.hpp>
#endif

#include "tool_common.hpp"

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
#include <exec/libdispatch_queue.hpp>
#endif

#if defined(BLAKE3PP_BENCH_UPSTREAM)
#include <blake3.h>

#include "kernel/kernel.hpp"

// The reference implementation's kernels, already present in the linked
// baseline library, wrapped as kernel_ops tables. That plugs them into the
// blake3pp pipeline, meaning subtree batching, the CV stack and the
// parallel engine, through the same seam every portable variant uses. Each
// reference row therefore differs from its same-ISA blake3pp twin ONLY in
// the hash_many kernel. Only the flag argument widths differ, since the
// reference narrows them to uint8_t.
//
// The comparison rows come from this:
//
//   - The portable C kernel, the scalar row's direct counterpart.
//   - Hand-scheduled AVX2 and AVX-512 assembly on x86-64, GAS-built under
//     gcc/clang/clang-cl and MASM under MSVC.
//   - NEON intrinsics on aarch64. The reference ships no ARM assembly.
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

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#endif
#endif

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
// the flag arguments back to the blake3pp signature. The hand-written
// kernels are assembly, which MemorySanitizer cannot instrument, so
// their output is marked initialized by hand or every byte of it reads
// as poisoned downstream.
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
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
  __msan_unpoison(out, num_inputs * blake3pp::kern::out_len);
#endif
#endif
}

// Everything that varies by host architecture, decided once: which wide
// kernels exist, which blake3pp variant each pairs with, and how the rows
// read. Narrowest first, so the widest AVAILABLE entry is the last match.
// That is the ISA the reference's own dispatcher would pick, and the one
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

// The interleaved passes print nothing until every pass has run, so a
// human at a terminal gets a pass counter on stderr; logs (CI captures,
// pipes) get silence instead of carriage-return soup.
bool stderr_is_tty() {
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#elif defined(__unix__) || defined(__APPLE__)
  return ::isatty(::fileno(stderr)) != 0;
#else
  return false;
#endif
}

// Why the t16 tuner can disagree with the table above, settled by
// measurement rather than argument. tune_transpose16() races the three
// strategies over a 16 KiB working set reused hundreds of times (L1
// resident throughout), while the benchmark rows stream 512 MiB out of
// DRAM. If the strategies rank differently in those two regimes, the tuner
// is calibrated for the wrong one.
//
// This sweeps the SAME comparison across working-set sizes, and runs each
// size in both strategy orders. A ranking that flips with size is a regime
// mismatch. A ranking that flips with ORDER is thermal or frequency drift
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
  // This line once claimed "its own race uses a 16 KiB working set",
  // which was only true of an old tuner revision. Print facts, not
  // memories: the race sizes itself to the tuned-for bytes (capped).
  println(stdout,
          "\n  tune_transpose16() picks: {}   (raced at its default "
          "{} MiB working set, interleaved best-of-3)",
          blake3pp::to_string(blake3pp::tune_transpose16()),
          blake3pp::default_tune_bytes >> 20);
  blake3pp::set_transpose16(saved);
}
// The parallel engine's split before its agents pulled parts from a
// counter, kept as the reference/static control row: at most 256
// power-of-two parts in one bulk call, so a provider that hands out fixed
// shares (stdexec's static_thread_pool does) joins on its slowest agent.
template <class Scheduler>
blake3pp::digest static_split_hash(std::span<const std::byte> input,
                                   Scheduler sched,
                                   const blake3pp::kern::kernel_ops* ops) {
  namespace ex = blake3pp::ex;
  constexpr std::size_t chunk = blake3pp::chunk_size;
  constexpr std::size_t max_parts = 256;
  blake3pp::hasher h{ops};
  const std::size_t safe_chunks =
      input.size() > chunk ? (input.size() - 1) / chunk : 0;
  const std::size_t part = std::bit_floor(
      std::max<std::size_t>((safe_chunks + max_parts - 1) / max_parts, 16));
  const std::size_t n_parts = safe_chunks / part;
  if (n_parts < 2) {
    h.update(input);
    return h.finalize();
  }
  std::vector<std::array<std::uint32_t, 8>> cvs(n_parts);
  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n_parts, [&](std::size_t i) noexcept {
                blake3pp::detail::compress_subtree_cv(
                    ops, input.data() + i * part * chunk, part,
                    static_cast<std::uint64_t>(i) * part, h.key_words(),
                    h.mode_flags(), cvs[i]);
              });
  ex::sync_wait(std::move(work));
  for (const auto& cv : cvs) {
    h.push_subtree_cv(cv, part);
  }
  h.update(input.subspan(n_parts * part * chunk));
  return h.finalize();
}

}  // namespace

int main(int argc, char** argv) {
  b3tool::tool_startup();
  // 512 MiB streams past every cache; on a small board (128 MB and no
  // swap: the LicheeRV Nano) that buffer belongs to the OOM killer, so
  // the default is a quarter of physical memory where that is less.
  std::size_t mib = 512;
  if (const std::uint64_t ram = b3tool::physical_memory_bytes(); ram != 0) {
    mib = std::min(mib, std::max<std::size_t>(1, static_cast<std::size_t>(ram / 4 >> 20)));
  }
  int reps = 5;
  double cooldown_s = 5.0;
  int pin_cpu = -1;
  unsigned pool_threads = b3tool::default_threads();
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
                 "idle seconds between interleaved passes (0 disables)")
      ->capture_default_str();
  app.add_option("--pin", pin_cpu,
                 "pin the single-thread rows to this CPU (the parallel rows "
                 "always run with the startup affinity mask restored)")
      ->check(CLI::NonNegativeNumber);
  app.add_option("--threads", pool_threads,
                 "parallel-engine threads (1 = a one-thread pool; default: all)")
      ->check(b3tool::at_least_one_thread)
      ->capture_default_str();
  app.add_flag("--t16-sweep", sweep_t16,
               "diagnose the AVX-512 transpose tuner: rank the three "
               "strategies across working-set sizes and strategy orders");

  app.add_option("arch", arches, "SIMD variants to measure")
      ->transform(b3tool::arch_transformer());
  CLI11_PARSE(app, argc, argv);

  // Startup affinity captured before any pin, so the parallel section can
  // restore exactly what the process was given (which on Android/cpuset
  // systems may already be less than the machine).
  b3tool::thread_pin placement;
  if (pin_cpu >= 0) {
    if (!b3tool::thread_pin::supported()) {
      println(stderr, "--pin: no thread-affinity API on this platform");
      return b3tool::exit_usage;
    }
    if (!placement.pin(pin_cpu)) {
      println(stderr,
              "--pin {}: pinning failed (CPU offline, or outside this "
              "process's cpuset?)",
              pin_cpu);
      return b3tool::exit_usage;
    }
  }

  if (sweep_t16) {
    t16_sweep(reps, cooldown_s);  // single-threaded throughout: --pin holds
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

  println(stdout,
          "blake3pp throughput, {} MiB, best of {} interleaved passes, "
          "{:.0f}s cooldown",
          mib, reps, cooldown_s);
  if (pin_cpu >= 0) {
    println(stdout, "single-thread rows pinned to cpu {}", pin_cpu);
  }
  println(stdout, "auto resolves to: {}\n",
          blake3pp::to_string(blake3pp::best_available()));

  b3tool::cooldown cooldown(cooldown_s);

  // Hashes the whole input with one kernel table, sequentially.
  const auto hash_with = [&](const blake3pp::kern::kernel_ops* ops) {
    blake3pp::hasher h{ops};
    h.update(std::span<const std::byte>{input});
    return h.finalize();
  };

  // The single-thread rows are REGISTERED first and measured afterwards,
  // interleaved (see the file comment): sections exist for printing only
  // and say nothing about execution order.
  struct bench_row {
    std::string label;
    std::string note;
    int width = 10;
    std::function<blake3pp::digest()> fn;  // null: info-only line
    std::string info;
    double best = 1e100;
    blake3pp::digest d{};
    std::set<int> cpus;  // every CPU any rep of this row ended on
  };
  struct bench_section {
    std::string title;
    std::vector<bench_row> rows;
    std::function<void()> footer;  // extra print under the rows
  };
  std::vector<bench_section> sections;
  const auto add_section = [&](std::string title) {
    sections.emplace_back();
    sections.back().title = std::move(title);
  };
  const auto add_row = [&](std::string label, std::string note,
                           std::function<blake3pp::digest()> fn,
                           int width = 10) {
    bench_row r;
    r.label = std::move(label);
    r.note = std::move(note);
    r.width = width;
    r.fn = std::move(fn);
    sections.back().rows.push_back(std::move(r));
  };

  add_section("blake3pp kernels (single thread)");
  for (const auto a : arches) {
    if (!blake3pp::is_available(a)) {
      bench_row r;
      r.label = std::string{blake3pp::to_string(a)};
      r.info = "unavailable on this machine";
      sections.back().rows.push_back(std::move(r));
      continue;
    }
    add_row(std::string{blake3pp::to_string(a)}, "",
            [&, a] { return hash_with(blake3pp::detail::resolve(a)); });
  }

  // The width-16 transpose strategies, raced in-process (the dial is
  // runtime; no per-strategy binaries needed), plus what the tuner picks.
  // Interleaved execution means the dial must be set per REP, inside fn,
  // and put back so the neighboring rows see the default.
  if (const auto w16 = first_w16_arch(); w16.has_value()) {
    const auto saved = blake3pp::active_transpose16();
    for (const auto strat :
         {blake3pp::transpose16::staging, blake3pp::transpose16::tree,
          blake3pp::transpose16::quartered}) {
      add_row(
          std::format("  t16-{}", blake3pp::to_string(strat)), "",
          [&, w16, strat, saved] {
            blake3pp::set_transpose16(strat);
            const auto d = hash_with(blake3pp::detail::resolve(*w16));
            blake3pp::set_transpose16(saved);
            return d;
          },
          14);
    }
    sections.back().footer = [&, saved] {
      cooldown();
      // Tuned for THIS run's input size, so the verdict is comparable to
      // the three rows above it. The winner depends on the size
      // (cache-resident inputs rank the strategies differently from
      // streaming ones), so a tuner asked about a different size than the
      // table measures has every right to disagree with it.
      println(stdout, "    t16 tuner picks: {} (tuned for {} MiB)",
              blake3pp::to_string(blake3pp::tune_transpose16(input.size())),
              mib);
      blake3pp::set_transpose16(saved);
    };
  }

#if defined(BLAKE3PP_BENCH_UPSTREAM)
  // The reference kernels behind the blake3pp kernel_ops seam. Identical
  // pipeline, only hash_many swapped, so any difference to the section
  // above is pure kernel codegen.
  //
  // Row labels match the blake3pp rows they pair with: portable <->
  // scalar, neon <-> neon, avx2 <-> avx2, avx512 <-> avx512. Always
  // compare a reference row against its same-ISA twin, never against the
  // end-to-end `reference` row, which picks its own best ISA.
  add_section(
      std::format("reference impl. kernels in the blake3pp pipeline "
                  "(single thread, BLAKE3 {})",
                  BLAKE3_VERSION_STRING));
  blake3pp::kern::kernel_ops port_ops =
      *blake3pp::detail::resolve(blake3pp::arch::scalar);
  port_ops.hash_many = &portable_hash_many;
  add_row("portable", "", [&] { return hash_with(&port_ops); });

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
    add_row(k.row, k.note, [&, k] {
      blake3pp::kern::kernel_ops ops = *blake3pp::detail::resolve(k.variant);
      ops.hash_many = k.hash_many;  // compress_in_place stays portable
      return hash_with(&ops);
    });
    asm_ops = *blake3pp::detail::resolve(k.variant);
    asm_ops.hash_many = k.hash_many;
    asm_row = k.row;
  }
#endif

  // The reference implementation as shipped: its own runtime dispatch, same
  // input. The digest must match every row above.
  add_section("reference impl. end-to-end (single thread, own dispatch)");
  add_row("reference", "", [&] {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, input.data(), input.size());
    blake3pp::digest d{};
    blake3_hasher_finalize(&h, blake3pp::kern::kernel_bytes(d.bytes.data()),
                           BLAKE3_OUT_LEN);
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
    // The reference dispatches to its assembly kernels internally.
    __msan_unpoison(d.bytes.data(), d.bytes.size());
#endif
#endif
    return d;
  });
#endif

  // The blake3pp full pipeline on a single thread. It discriminates
  // between "the pool scales badly" and "the subtree and CV machinery
  // costs more than the raw kernel loop at this width".
  //
  // The reference end-to-end row above has had this mirror all along.
  // This row was the blind spot while chasing the width-16 pool anomaly.
  add_section("blake3pp end-to-end (single thread)");
  add_row("blake3pp", "", [&] {
    return blake3pp::hash(std::span<const std::byte>{input});
  });

  // --- measurement: rep-outer, row-inner, start row rotated per pass ---
  // Pass -1 is the uncounted warmup. Cooldown idles between PASSES, not
  // rows: an idle gap between rows is exactly what let EAS reset its
  // upmigration on the Exynos, handing the post-gap row a different core
  // class than its neighbors. Rotation gives every row every position in
  // the pass order across reps, so whatever thermal/placement drift a
  // position carries is spread evenly before best-of picks the ceiling.
  std::vector<bench_row*> flat;
  for (auto& s : sections) {
    for (auto& r : s.rows) {
      if (r.fn) {
        flat.push_back(&r);
      }
    }
  }
  const bool progress = stderr_is_tty();
  for (int pass = 0; pass <= reps && !flat.empty(); ++pass) {
    cooldown();
    if (progress) {
      b3tool::print(stderr, "\r[pass {}/{}{}] ", pass + 1, reps + 1,
                    pass == 0 ? ", warmup" : "");
      std::fflush(stderr);
    }
    for (std::size_t i = 0; i < flat.size(); ++i) {
      bench_row& r = *flat[(i + static_cast<std::size_t>(pass)) % flat.size()];
      const auto t0 = std::chrono::steady_clock::now();
      r.d = r.fn();
      const auto t1 = std::chrono::steady_clock::now();
      if (const int cpu = b3tool::current_cpu(); cpu >= 0) {
        r.cpus.insert(cpu);
      }
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (pass > 0 && s < r.best) {
        r.best = s;
      }
    }
  }
  if (progress) {
    b3tool::print(stderr, "\r{:24}\r", "");
    std::fflush(stderr);
  }

  const auto join_cpus = [](const std::set<int>& cpus) {
    std::string out;
    for (const int c : cpus) {
      out += out.empty() ? std::format("{}", c) : std::format(",{}", c);
    }
    return out;
  };

  std::set<int> cpus_seen;
  bool first_section = true;
  for (auto& s : sections) {
    println(stdout, "{}{}", first_section ? "" : "\n", s.title);
    first_section = false;
    for (auto& r : s.rows) {
      if (!r.fn) {
        println(stdout, "  {:<{}} {}", r.label, r.width, r.info);
        continue;
      }
      cpus_seen.insert(r.cpus.begin(), r.cpus.end());
      std::string note{r.note};
      if (!r.cpus.empty()) {
        note += std::format("{}cpu {}", note.empty() ? "" : ", ",
                            join_cpus(r.cpus));
      }
      const std::string rate = b3tool::rate(input.size(), r.best);
      if (note.empty()) {
        println(stdout, "  {:<{}} {}   ({}...)", r.label, r.width, rate,
                r.d.to_hex().substr(0, 16));
      } else {
        println(stdout, "  {:<{}} {}   ({}...)  [{}]", r.label, r.width,
                rate, r.d.to_hex().substr(0, 16), note);
      }
    }
    if (s.footer) {
      s.footer();
    }
  }
  if (cpus_seen.size() > 1) {
    println(stdout,
            "\n  note: the single-thread rows ran on {} different CPUs "
            "({}). On machines with more than one core class such rows "
            "are not comparable; rerun with --pin <cpu>.",
            cpus_seen.size(), join_cpus(cpus_seen));
  }

  // The sender-based parallel engine over the whole tree: the numbers that
  // matter for feeding modern storage. Rows are kernel/scheduler pairings.
  // --threads is honored exactly under stdexec (an owned pool of that
  // size); other providers fall back to the process-wide scheduler.
  // The pool must see the machine, not the pin: restore the startup mask
  // BEFORE the pool is constructed, since on Linux new threads inherit the
  // creating thread's affinity, so a leaked --pin 9 would silently turn
  // the N-thread rows into one-core numbers. The affinity count in the
  // header is the receipt (and also exposes an Android cpuset that hands
  // the process fewer CPUs than the machine has).
  placement.restore();
  const unsigned nthreads = pool_threads;
  const int affinity = b3tool::affinity_cpu_count();
  println(stdout, "\nparallel engine ({} threads{})", nthreads,
          affinity > 0 ? std::format(", affinity mask: {} cpus", affinity)
                       : std::string{});

  // A parallel table row, measured and printed immediately (interleaving
  // exists for the single-thread comparisons; these rows use every core by
  // construction, so placement symmetry is moot).
  const auto row = [&](std::string_view label, std::string_view note,
                       auto&& fn, int width = 15) {
    cooldown();
    blake3pp::digest d{};
    const double best =
        b3tool::best_seconds(reps, /*warmup=*/true, [&] { d = fn(); });
    println(stdout, "  {:<{}} {}   ({}...)  [{}]", label, width,
            b3tool::rate(input.size(), best), d.to_hex().substr(0, 16), note);
  };

  // An OWNED pool at exactly nthreads, not b3tool::compute_pool, whose
  // scheduler() has a threads>1 precondition (--threads 1 is a valid and
  // interesting measurement here: pool machinery at zero parallelism).
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
  // The process scheduler, sized here for every provider, which is what
  // makes --threads mean the same thing in a stdexec and a beman build.
  blake3pp::size_parallel_scheduler(nthreads);
#elif defined(BLAKE3PP_EXECUTION_STDEXEC)
  exec::static_thread_pool engine_pool{nthreads};
#endif
  auto engine_sched =
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
      blake3pp::get_parallel_scheduler();
#elif defined(BLAKE3PP_EXECUTION_STDEXEC)
      engine_pool.get_scheduler();
#else
      blake3pp::get_parallel_scheduler();
#endif
  {
    auto sched = engine_sched;
    row("blake3pp/pool",
        std::format("{} pool", blake3pp::execution_provider()),
        [&] { return blake3pp::hash(std::span<const std::byte>{input}, sched); });
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
    // The same kernel and pool under the engine's former split, as the
    // control for the counter-pulled parts in the row above.
    row("reference/static",
        std::format("{} kernel, 256 parts, fixed shares", asm_row),
        [&] {
          return static_split_hash(std::span<const std::byte>{input}, sched,
                                   &asm_ops);
        },
        15);
  }
#endif
  return 0;
}
