// The browser module: the library's types behind embind, under their own
// names. hasher, parallel_hasher and output_reader keep the member
// functions they have in C++; the one-shot hash() has both its shapes;
// the dispatch queries are the ones blake3ppsum --version prints. What
// the boundary changes: bytes are passed as (address, length) into the
// module's memory, which the embedder gets from Module._malloc and
// writes through Module.HEAPU8 (JavaScript cannot hand over a span);
// digests come back as hex; and a scheduler is an object, thread_pool,
// which the embedder owns and passes exactly as C++ code passes a
// scheduler (it must outlive every parallel_hasher made from it).
//
// Every call runs on the thread that instantiated the module and a
// pooled call blocks it until the pool is done, so a page puts the
// module in a worker, where waiting is allowed.
#include <blake3pp/blake3pp.hpp>

#include <emscripten/bind.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#if defined(BLAKE3PP_EXECUTION_STDEXEC)
#include <exec/static_thread_pool.hpp>
#endif

namespace {

using namespace blake3pp;

std::span<const std::byte> in(std::uintptr_t ptr, std::size_t len) {
  return {std::bit_cast<const std::byte*>(ptr), len};
}
std::span<std::byte> out(std::uintptr_t ptr, std::size_t len) {
  return {std::bit_cast<std::byte*>(ptr), len};
}
arch arch_named(const std::string& name) {
  return arch_from_string(name).value_or(arch::auto_detect);
}

/// A pool of exactly this many threads (exec::static_thread_pool under
/// stdexec; the process-wide parallel scheduler under the other
/// providers, as the CLI tools do), owned by the embedder.
class thread_pool {
 public:
  explicit thread_pool(unsigned threads)
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
      : pool_(threads)
#endif
  {
    (void)threads;
  }
  parallel_scheduler_t scheduler() {
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
    return pool_.get_scheduler();
#else
    return get_parallel_scheduler();
#endif
  }

 private:
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
  exec::static_thread_pool pool_;
#endif
};

using pooled_hasher = parallel_hasher<parallel_scheduler_t>;

// hasher
hasher make_hasher(const std::string& variant) { return hasher{arch_named(variant)}; }
void hasher_update(hasher& h, std::uintptr_t ptr, std::size_t len) { h.update(in(ptr, len)); }
void hasher_update_text(hasher& h, const std::string& text) { h.update(text); }
std::string hasher_finalize(const hasher& h) { return h.finalize().to_hex(); }
std::string hasher_selected_arch(const hasher& h) { return to_string(h.selected_arch()); }
// Counts and offsets are doubles on the JavaScript side (exact to 2^53,
// which no message in a browser reaches) rather than BigInts.
double hasher_count(const hasher& h) { return static_cast<double>(h.count()); }

// parallel_hasher
pooled_hasher make_parallel_hasher(thread_pool& pool, const std::string& variant) {
  return pooled_hasher{pool.scheduler(), parallel_hasher_options{.a = arch_named(variant)}};
}
void ph_update(pooled_hasher& h, std::uintptr_t ptr, std::size_t len) { h.update(in(ptr, len)); }
void ph_update_text(pooled_hasher& h, const std::string& text) { h.update(text); }
std::string ph_finalize(const pooled_hasher& h) { return h.finalize().to_hex(); }
double ph_count(const pooled_hasher& h) { return static_cast<double>(h.count()); }

// output_reader
void reader_fill(output_reader& r, std::uintptr_t ptr, std::size_t len) { r.fill(out(ptr, len)); }
void reader_seek(output_reader& r, double byte_offset) {
  r.seek(static_cast<std::uint64_t>(byte_offset));
}
double reader_position(const output_reader& r) { return static_cast<double>(r.position()); }

// the one-shots
std::string hash_seq(std::uintptr_t ptr, std::size_t len, const std::string& variant) {
  hasher h{arch_named(variant)};
  h.update(in(ptr, len));
  return h.finalize().to_hex();
}
std::string hash_pooled(std::uintptr_t ptr, std::size_t len, thread_pool& pool,
                        const std::string& variant) {
  return hash(in(ptr, len), pool.scheduler(), arch_named(variant)).to_hex();
}

// dispatch
std::string version_() { return std::string{version()}; }
std::string simd_provider_() { return std::string{simd_provider()}; }
std::string execution_provider_() { return std::string{execution_provider()}; }
emscripten::val names(std::span<const arch> arches) {
  auto out = emscripten::val::array();
  for (const auto a : arches) {
    out.call<void>("push", std::string{to_string(a)});
  }
  return out;
}
emscripten::val compiled_arches_() { return names(compiled_arches()); }
emscripten::val available_arches_() { return names(available_arches()); }
std::string best_available_() { return to_string(best_available()); }

}  // namespace

EMSCRIPTEN_BINDINGS(blake3pp) {
  using namespace emscripten;
  class_<hasher>("hasher")
      .constructor(&make_hasher)
      .function("update", &hasher_update)
      .function("update", &hasher_update_text)
      .function("finalize", &hasher_finalize)
      .function("finalize_xof", &hasher::finalize_xof)
      .function("reset", &hasher::reset)
      .function("count", &hasher_count)
      .function("selected_arch", &hasher_selected_arch);
  class_<output_reader>("output_reader")
      .function("fill", &reader_fill)
      .function("seek", &reader_seek)
      .function("position", &reader_position);
  class_<thread_pool>("thread_pool").constructor<unsigned>();
  class_<pooled_hasher>("parallel_hasher")
      .constructor(&make_parallel_hasher)
      .function("update", &ph_update)
      .function("update", &ph_update_text)
      .function("finalize", &ph_finalize)
      .function("reset", &pooled_hasher::reset)
      .function("count", &ph_count);
  function("hash", &hash_seq);
  function("hash", &hash_pooled);
  function("version", &version_);
  function("simd_provider", &simd_provider_);
  function("execution_provider", &execution_provider_);
  function("compiled_arches", &compiled_arches_);
  function("available_arches", &available_arches_);
  function("best_available", &best_available_);
}
