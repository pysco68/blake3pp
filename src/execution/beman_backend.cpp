// The program-level backend behind beman.execution's parallel_scheduler
// (P2079 replaceability): beman's standard-shaped front end, stdexec's
// static_thread_pool as the engine room. Scaffolding by design: beman's
// own default backend is still TODO upstream (their header says so); when
// it ships, this TU and the blake3pp::beman_backend target get deleted and
// nothing else changes.
//
// A backend definition is one-per-program (like a global allocator), which
// is why this lives in its own opt-in library target rather than inside
// blake3pp itself: downstream applications may want to own the hook.
//
// Semantics mirror beman's reference test backend:
//   - schedule():             complete via set_value() on a pool thread
//   - schedule_bulk_chunked:  backend picks chunking; execute(begin, end)
//                             per chunk, then exactly one set_value() after
//                             all chunks (set_error on submission failure)
//   - schedule_bulk_unchunked: execute(i, i + 1) per item, same completion
// The preallocated-storage span is an optimization hint we don't use;
// operation state lives in start_detached's allocation instead.

#include <beman/execution/execution.hpp>

#include <exec/start_detached.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <thread>

namespace repl = beman::execution::parallel_scheduler_replacement;

namespace {

exec::static_thread_pool& pool() {
  static exec::static_thread_pool p{std::thread::hardware_concurrency()};
  return p;
}

struct stdexec_backend final : repl::parallel_scheduler_backend {
  void schedule(repl::receiver_proxy& proxy,
                std::span<std::byte>) noexcept override {
    try {
      exec::start_detached(
          stdexec::schedule(pool().get_scheduler()) |
          stdexec::then([&proxy]() noexcept { proxy.set_value(); }));
    } catch (...) {
      proxy.set_error(std::current_exception());
    }
  }

  void schedule_bulk_chunked(std::size_t shape,
                             repl::bulk_item_receiver_proxy& proxy,
                             std::span<std::byte> storage) noexcept override {
    const std::size_t threads =
        std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
    schedule_bulk(shape, (shape + threads - 1) / threads, proxy, storage);
  }

  void schedule_bulk_unchunked(std::size_t shape,
                               repl::bulk_item_receiver_proxy& proxy,
                               std::span<std::byte> storage) noexcept override {
    schedule_bulk(shape, 1, proxy, storage);
  }

 private:
  void schedule_bulk(std::size_t shape, std::size_t chunk_length,
                     repl::bulk_item_receiver_proxy& proxy,
                     std::span<std::byte> storage) noexcept {
    if (shape == 0) {
      schedule(proxy, storage);
      return;
    }
    const std::size_t chunks = (shape + chunk_length - 1) / chunk_length;
    try {
      exec::start_detached(
          stdexec::schedule(pool().get_scheduler()) |
          stdexec::bulk(stdexec::par, chunks,
                        [&proxy, chunk_length, shape](std::size_t i) noexcept {
                          const std::size_t begin = i * chunk_length;
                          const std::size_t end =
                              std::min(begin + chunk_length, shape);
                          proxy.execute(begin, end);
                        }) |
          stdexec::then([&proxy]() noexcept { proxy.set_value(); }));
    } catch (...) {
      proxy.set_error(std::current_exception());
    }
  }
};

}  // namespace

namespace beman::execution::parallel_scheduler_replacement {

auto query_parallel_scheduler_backend()
    -> std::shared_ptr<parallel_scheduler_backend> {
  static auto backend = std::make_shared<stdexec_backend>();
  return backend;
}

}  // namespace beman::execution::parallel_scheduler_replacement
