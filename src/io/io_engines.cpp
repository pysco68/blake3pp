// I/O engine introspection: what this binary carries, what the kernel
// grants, and which engine a fresh open uses.
//
// This is the dispatch side's arrangement one layer down, with one
// difference that decides the whole file. A kernel variant is in the
// binary because the build registered it, so dispatch can ask the
// registry. An I/O engine is in the binary because a backend header was
// compiled, and there is no registry. The first version of this file
// wrote the per-platform list down by hand and got Windows wrong: it
// named the I/O ring unconditionally while the ring lives behind
// `#if __has_include(<ioringapi.h>)`, so a build without the SDK header
// reported an engine that was not in it.
//
// What replaces the list is the ladder the layer already had. Every
// backend degrades, and src/io/engine_rung.hpp turns that into two
// declarations per context: the engine it is, and the rung it defers
// to. From those:
//
//   * The engines this binary carries are the chain of `rung_below`
//     from native_io_context. Nothing states the list twice, so nothing
//     can state it differently.
//   * Whether the kernel grants them is not decided here. The backends
//     decide it in their constructors (a kqueue plus a non-zero
//     vfs.aio.max_aio_queue_per_proc, an io_uring_setup(), an
//     IORING_VERSION_3) and report which rung they landed on. This
//     constructs one context with the smallest resources it will take
//     and reads that back. A context engaged rung N because everything
//     above it was refused and everything below is what it would have
//     degraded to next, so the chain from rung N down is the set the
//     kernel grants, and rung N is the best engine.
//   * The enumerators, their two spellings, the preference ranks and
//     the async column come from include/blake3pp/detail/io_engine.def,
//     the list the public enum is generated from. The ranks are what
//     the chain is checked against below, at compile time.
//
// The per-file half is elsewhere. Whether a path's filesystem grants
// the cache bypass, and whether one open keeps the engine, need a file:
// that is probe_file_io().

#include <blake3pp/io.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "io/backend.hpp"
#include "io/backend_select.hpp"
#include "io/engine_rung.hpp"

namespace blake3pp {
namespace {

// The io_engine.def rows: enum value, the two spellings, preference
// rank, and whether the OS carries the transfer rather than the calling
// thread.
struct engine_row {
  io_engine e;
  const char* read_name;
  const char* write_name;
  unsigned rank;
  bool async;
};

constexpr engine_row engine_table[] = {
#define BLAKE3PP_IO_ENGINE(enumerator, read_name, write_name, rank, async) \
  {io_engine::enumerator, read_name, write_name, rank, (async) != 0},
#include <blake3pp/detail/io_engine.def>
#undef BLAKE3PP_IO_ENGINE
};

constexpr std::size_t num_engines = std::size(engine_table);

constexpr const engine_row* row_for(io_engine e) noexcept {
  for (const engine_row& r : engine_table) {
    if (r.e == e) {
      return &r;
    }
  }
  return nullptr;
}

constexpr unsigned rank_of(io_engine e) noexcept {
  const engine_row* const r = row_for(e);
  return r != nullptr ? r->rank : 0u;
}

constexpr bool async_engine(io_engine e) noexcept {
  const engine_row* const r = row_for(e);
  return r != nullptr && r->async;
}

consteval bool engine_ranks_unique() {
  for (std::size_t i = 0; i < num_engines; ++i) {
    for (std::size_t j = i + 1; j < num_engines; ++j) {
      if (engine_table[i].rank == engine_table[j].rank) {
        return false;
      }
    }
  }
  return true;
}
static_assert(engine_ranks_unique(), "io_engine.def ranks must be unique");

// ---------------------------------------------------------------------
// The ladder, walked.

namespace rung = detail::io_impl;

// How many rungs the native context's chain has, and the engines along
// it. The recursion ends at the floor, whose `rung_below` is void: a
// synchronous rung served from a context's own deferred_ list, declared
// in engine_rung.hpp because there is no object to ask.
template <class R>
consteval std::size_t rung_depth() {
  if constexpr (std::is_void_v<typename R::rung_below>) {
    return 1;
  } else {
    return 1 + rung_depth<typename R::rung_below>();
  }
}

template <class R, std::size_t N>
constexpr void write_rungs(std::array<io_engine, N>& out,
                           std::size_t at) noexcept {
  out[at] = R::rung_engine;
  if constexpr (!std::is_void_v<typename R::rung_below>) {
    write_rungs<typename R::rung_below>(out, at + 1);
  }
}

// The engines this binary carries, best-first, because that is what
// "the rung below" means. This is the list compiled_io_engines()
// returns, and it is not written anywhere: it is read off the types the
// platform branch of backend_select.hpp actually selected.
constexpr std::array<io_engine, rung_depth<rung::native_io_context>()>
    ladder = [] {
      std::array<io_engine, rung_depth<rung::native_io_context>()> out{};
      write_rungs<rung::native_io_context>(out, 0);
      return out;
    }();

// The invariants, as static_asserts. This file used to assert them at
// runtime, and -DNDEBUG is in every lane of the real build, so those
// never executed anywhere.

// A ladder descends: each rung defers to one the table ranks below it.
// That is what makes the chain from the engaged rung down equal to the
// set of compiled engines this kernel grants, and the chain's order the
// preference order the API promises. It also rules out a cycle, which
// would otherwise be an infinite template recursion with no diagnosis.
consteval bool ladder_descends() {
  for (std::size_t i = 1; i < ladder.size(); ++i) {
    if (rank_of(ladder[i]) <= rank_of(ladder[i - 1])) {
      return false;
    }
  }
  return true;
}
static_assert(ladder_descends(),
              "each rung must defer to one the .def ranks below it");

// Nothing hangs below a synchronous rung, so a refusal anywhere on the
// ladder lands on a rung that does the transfer on the calling thread.
// The old file meant this when it returned `floor_engine` as the
// "async was refused" answer, where it held only by accident of the
// ranks.
consteval bool sync_rungs_are_final() {
  bool seen_sync = false;
  for (const io_engine e : ladder) {
    if (seen_sync && async_engine(e)) {
      return false;
    }
    seen_sync = seen_sync || !async_engine(e);
  }
  return true;
}
static_assert(sync_rungs_are_final(),
              "no async rung may sit below a synchronous one");

// The floor is synchronous, which is what makes available_io_engines()
// non-empty by construction: there is no kernel that refuses pread().
static_assert(!ladder.empty(), "a platform must compile at least one engine");
static_assert(!async_engine(ladder.back()),
              "the last rung must be the synchronous one nothing refuses");

// ---------------------------------------------------------------------
// The one question that needs the OS.

// Construct the platform's own context the way a fresh open constructs
// it -- default options, the smallest queue it will take -- and read
// back which rung it landed on. The context is destroyed immediately;
// only the verdict is kept.
rung::rung_verdict probe_engaged_rung() noexcept {
  try {
    const rung::native_io_context ctx(rung::reader_context_options{}, 1);
    return ctx.engaged_rung();
  } catch (...) {
    // No context could be constructed at all: a completion port
    // Windows would not create, an eventfd there was no descriptor
    // for. The synchronous rung needs nothing, so that is the answer,
    // and it is not settled -- a resource failure, and the next caller
    // may get the engine. This clause used to cache it, which reported
    // that a machine had no completion port because one CreateEvent
    // failed.
    return {ladder.back(), false};
  }
}

// The engine a fresh open uses. Probed once if the answer is settled,
// on every call if it is not.
//
// The question is what may be remembered. Dispatch can cache its CPU
// probe forever; a process cannot grow an instruction set. An I/O
// engine is refused for reasons that expire. The backend says which
// kind its verdict is -- a standing fact about this kernel (ENOSYS,
// EPERM, no <ioringapi.h> on this Windows) or about this moment
// (EMFILE, ENOMEM, a sysctl an administrator may raise) -- and only
// the first is kept. The unsettled case costs one io_uring_setup() or
// one kqueue() per call, and only on a machine that refuses the
// engine.
io_engine engaged_engine() noexcept {
  // Relaxed throughout: the value is idempotent, every writer computes
  // the same answer for a settled verdict, and a racing reader that
  // misses it merely probes again.
  static constexpr std::uint8_t unprobed = 0xffu;
  static std::atomic<std::uint8_t> settled{unprobed};
  const std::uint8_t cached = settled.load(std::memory_order_relaxed);
  if (cached != unprobed) {
    return static_cast<io_engine>(cached);
  }
  const rung::rung_verdict v = probe_engaged_rung();
  if (v.settled) {
    settled.store(static_cast<std::uint8_t>(v.engine),
                  std::memory_order_relaxed);
  }
  return v.engine;
}

}  // namespace

bool is_available(io_engine e) noexcept {
  const std::span<const io_engine> granted = available_io_engines();
  for (const io_engine g : granted) {
    if (g == e) {
      return true;
    }
  }
  return false;
}

std::span<const io_engine> compiled_io_engines() noexcept { return ladder; }

std::span<const io_engine> available_io_engines() noexcept {
  // The ladder from the engaged rung down. A context engaged that rung
  // because everything above it was refused, and everything below it is
  // what it would have degraded to in turn, so the suffix IS the set
  // this kernel grants -- in the order the API promises, without a
  // second pass over anything.
  const io_engine best = engaged_engine();
  const std::span<const io_engine> all{ladder};
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i] == best) {
      return all.subspan(i);
    }
  }
  // Unreachable: the verdict is one of the ladder's own rungs. The
  // floor is the answer that is always true if it ever is reached.
  return all.subspan(all.size() - 1);
}

io_engine best_io_engine() noexcept { return engaged_engine(); }

const char* to_string(io_engine e, io_direction d) noexcept {
  if (const engine_row* r = row_for(e); r != nullptr) {
    return d == io_direction::write ? r->write_name : r->read_name;
  }
  return "unknown";
}

std::optional<io_engine> io_engine_from_string(std::string_view name) noexcept {
  for (const engine_row& r : engine_table) {
    if (name == r.read_name || name == r.write_name) {
      return r.e;
    }
  }
  return std::nullopt;
}

bool is_async(io_engine e) noexcept { return async_engine(e); }

}  // namespace blake3pp
