// Runtime architecture routing. Selection stays a plain pointer to a
// constexpr-initialized POD table: no heap, no vtable, no ifunc.
//
// Nothing here is hand-maintained in parallel with anything else:
//  * WHICH variants exist in this binary comes from the build-generated
//    blake3pp_kernel_registry.inc (one entry per blake3pp_add_kernel()
//    call), expanded below into extern declarations and the registry.
//  * WHAT variants exist at all, their names, and the preference ranking
//    come from include/blake3pp/detail/arch.def, the same list the
//    public enum is generated from.
//  * WHETHER this CPU can run one lives in the per-platform probe TUs,
//    cpu_detect_{x86,arm,riscv}.cpp, behind cpu_detect.hpp.
// Register a kernel in CMake, add its line to arch.def, answer for it in
// the right probe: each fact is stated exactly once.

#include <blake3pp/dispatch.hpp>

#include "blake3pp_version_stamp.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "dispatch/cpu_detect.hpp"
#include "kernel/kernel.hpp"

namespace blake3pp {

namespace kern {
#define BLAKE3PP_KERNEL(ns) \
  namespace ns {            \
  extern const kernel_ops ops; \
  }
#include "blake3pp_kernel_registry.inc"
#undef BLAKE3PP_KERNEL
}  // namespace kern

namespace {

struct registry_entry {
  arch a;
  const kern::kernel_ops* ops;
};

constexpr registry_entry registry[] = {
#define BLAKE3PP_KERNEL(ns) {arch::ns, &kern::ns::ops},
#include "blake3pp_kernel_registry.inc"
#undef BLAKE3PP_KERNEL
};

constexpr std::size_t num_kernels = std::size(registry);

// The arch.def rows: enum value, canonical name, preference rank.
struct arch_row {
  arch a;
  const char* name;
  unsigned rank;
};

constexpr arch_row arch_table[] = {
#define BLAKE3PP_ARCH(enumerator, name, rank) {arch::enumerator, name, rank},
#include <blake3pp/detail/arch.def>
#undef BLAKE3PP_ARCH
};

constexpr std::size_t num_arches = std::size(arch_table);

consteval bool ranks_unique() {
  for (std::size_t i = 0; i < num_arches; ++i) {
    for (std::size_t j = i + 1; j < num_arches; ++j) {
      if (arch_table[i].rank == arch_table[j].rank) {
        return false;
      }
    }
  }
  return true;
}
static_assert(ranks_unique(), "arch.def ranks must be unique");

// Every enumerator sorted by rank: auto_detect first (rank 0), then
// best-first. This is all_arches() verbatim, and the dispatch preference
// order is its tail. One list, two roles.
constexpr std::array<arch, num_arches> all_enumerators = [] {
  std::array<arch, num_arches> out{};
  std::size_t n = 0;
  // Selection by ascending rank; ranks are unique (asserted above).
  for (unsigned last = 0; n < num_arches;) {
    const arch_row* next = nullptr;
    for (const arch_row& r : arch_table) {
      if ((n == 0 || r.rank > last) &&
          (next == nullptr || r.rank < next->rank)) {
        next = &r;
      }
    }
    out[n++] = next->a;
    last = next->rank;
  }
  return out;
}();
static_assert(all_enumerators.front() == arch::auto_detect,
              "auto_detect must hold the lowest rank");
static_assert(all_enumerators.back() == arch::scalar,
              "scalar must hold the highest rank");

constexpr std::span<const arch> preference =
    std::span{all_enumerators}.subspan(1);

// The compiled variants, sorted best-first, computed at compile time.
constexpr std::array<arch, num_kernels> compiled_sorted = [] {
  std::array<arch, num_kernels> out{};
  std::size_t i = 0;
  for (const arch p : preference) {
    for (const registry_entry& e : registry) {
      if (e.a == p) {
        out[i++] = p;
      }
    }
  }
  return out;
}();
static_assert(compiled_sorted.back() == arch::scalar,
              "the scalar fallback must always be registered");

bool compiled_in(arch a) noexcept {
  for (const registry_entry& e : registry) {
    if (e.a == a) {
      return true;
    }
  }
  return false;
}

bool cpu_supports_impl(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
      return true;
#if defined(__wasm__)
    case arch::simd128:
      // Module-level feature: SIMD opcodes in a module make load-time
      // validation the availability check; running code proves support.
      return true;
#endif
    default:
#if defined(BLAKE3PP_HAS_CPU_DETECT)
      return detail::platform_cpu_supports(a);
#else
      return false;
#endif
  }
}

struct avail_table {
  std::array<arch, num_kernels> entries{};
  std::size_t count = 0;
};

avail_table build_available() noexcept {
  avail_table t;
  for (const arch a : compiled_sorted) {
    if (cpu_supports_impl(a)) {
      t.entries[t.count++] = a;
    }
  }
  return t;
}

// The base table is built once; run_trap_probes() may publish an
// upgraded rebuild through the atomic pointer. Both live in static
// storage, so the returned span never dangles, and readers see either
// snapshot as one consistent unit.
avail_table g_avail_upgraded;
std::atomic<const avail_table*> g_avail_active{nullptr};

std::span<const arch> available_impl() noexcept {
  static const avail_table base = build_available();
  const avail_table* p = g_avail_active.load(std::memory_order_acquire);
  const avail_table& t = p != nullptr ? *p : base;
  return {t.entries.data(), t.count};
}

}  // namespace

bool is_available(arch a) noexcept {
  // auto_detect names whatever best_available() resolves to, which always
  // exists, since scalar is compiled into every build.
  return a == arch::auto_detect || (compiled_in(a) && cpu_supports_impl(a));
}

bool run_trap_probes() noexcept {
#if defined(BLAKE3PP_HAS_CPU_DETECT)
  // The magic static serializes concurrent callers and makes the call
  // idempotent; the platform hook itself is once-guarded too.
  static const bool changed = [] {
    if (!detail::platform_run_trap_probes()) {
      return false;
    }
    g_avail_upgraded = build_available();
    g_avail_active.store(&g_avail_upgraded, std::memory_order_release);
    return true;
  }();
  return changed;
#else
  return false;
#endif
}

bool cpu_supports(arch a) noexcept { return cpu_supports_impl(a); }

arch best_available() noexcept { return available_impl().front(); }

std::span<const arch> compiled_arches() noexcept { return compiled_sorted; }

std::span<const arch> available_arches() noexcept { return available_impl(); }

std::span<const arch> all_arches() noexcept { return all_enumerators; }

std::string_view version() noexcept { return BLAKE3PP_STAMPED_VERSION; }

std::string_view simd_provider() noexcept {
#if defined(BLAKE3PP_HAS_STD_SIMD)
  return "std::simd";
#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
  return "std::experimental::simd";
#else
  return "xsimd";
#endif
}

std::string_view execution_provider() noexcept {
#if defined(BLAKE3PP_EXECUTION_STD)
  return "std::execution";
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
  return "beman.execution";
#elif defined(BLAKE3PP_EXECUTION_STDEXEC)
  return "stdexec";
#else
  return "none";
#endif
}

const char* to_string(arch a) noexcept {
  for (const arch_row& r : arch_table) {
    if (r.a == a) {
      return r.name;
    }
  }
  return "unknown";
}

std::optional<arch> arch_from_string(std::string_view name) noexcept {
  for (const arch_row& r : arch_table) {
    if (name == r.name) {
      return r.a;
    }
  }
  return std::nullopt;
}

namespace detail {

const kern::kernel_ops* resolve(arch a) noexcept {
  if (a == arch::auto_detect || !cpu_supports_impl(a)) {
    a = best_available();
  }
  for (const registry_entry& e : registry) {
    if (e.a == a) {
      return e.ops;
    }
  }
  // Requested variant not compiled in: fall back to the best one that is.
  for (const arch b : available_impl()) {
    for (const registry_entry& e : registry) {
      if (e.a == b) {
        return e.ops;
      }
    }
  }
  return &kern::scalar::ops;  // unreachable: scalar is always registered
}

}  // namespace detail
}  // namespace blake3pp
