// The opt-in sized parallel scheduler (<blake3pp/parallel_backend.hpp>).
//
// P2079's scheduler is process-wide and unsized. The only lever the design
// offers is replacing the backend behind it, once per program, which is
// what this TU does.
//
// It is a separate target for that reason: one definition per program, like
// a global allocator. A program that never asks for it never links a thread
// pool.
//
// Per provider:
//   stdexec - the process pool is ours already; recording the size before
//             it is constructed is the whole job.
//   beman   - beman's own default backend is compiled out (the library is
//             configured with BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND=OFF
//             when BLAKE3PP_SIZED_PARALLEL_SCHEDULER is on) and this file
//             supplies the replacement, over the pool below.
// The std provider has no implementation here: no standard library ships
// std::execution yet, so there is nothing to compile it against.

#include <blake3pp/parallel_backend.hpp>

#include <blake3pp/parallel.hpp>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(BLAKE3PP_EXECUTION_BEMAN)
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>
#endif

namespace blake3pp {
namespace {

// The size the program asked for; 0 until it asks. Guarded by its own
// mutex rather than made atomic, because "fix it, but only once, and only
// before anything used it" is a two-field decision.
std::mutex g_mutex;
// Written under g_mutex, whose purpose is the check-and-set in
// size_parallel_scheduler, and read without it, so the noexcept reader
// cannot hit a lock failure and terminate.
std::atomic<unsigned> g_threads{0};

// Whether the scheduler exists yet. Under stdexec the pool itself records
// it (parallel.hpp); under beman the backend below is the moment.
std::atomic<bool> g_started{false};

bool scheduler_started() noexcept {
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
  return detail::process_pool_started().load(std::memory_order_relaxed);
#else
  return g_started.load(std::memory_order_relaxed);
#endif
}

[[noreturn]] void already(const std::string& what) {
  throw std::logic_error("blake3pp::size_parallel_scheduler: " + what);
}

}  // namespace
}  // namespace blake3pp

#if defined(BLAKE3PP_EXECUTION_BEMAN)

namespace repl = beman::execution::parallel_scheduler_replacement;

namespace blake3pp {
namespace {

// One job, run by up to `workers` pool threads, which delete it when the
// last of them is done. Raw ownership rather than shared_ptr: the queue
// holds pointers, so a task costs no allocation of its own.
struct job {
  std::atomic<std::size_t> running;  // workers + 1: the submitter's guard
  explicit job(std::size_t n) noexcept : running(n + 1) {}
  virtual ~job() = default;
  virtual void work() noexcept {}
  virtual void complete() noexcept = 0;
  // Returns true when this call was the last one out.
  bool release(std::size_t n = 1) noexcept {
    return running.fetch_sub(n, std::memory_order_acq_rel) == n;
  }
};

class pool {
 public:
  explicit pool(unsigned threads) {
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
      workers_.emplace_back([this](std::stop_token stop) { run(stop); });
    }
  }
  // jthread requests the stop and joins; condition_variable_any is what
  // makes the request reach a waiting worker at all.
  ~pool() = default;
  pool(const pool&) = delete;
  pool& operator=(const pool&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

  // Queues `j` for `count` workers and returns how many were queued; a
  // short answer means the queue could not grow, and the caller adjusts
  // the job's guard rather than losing the completion.
  std::size_t submit(job* j, std::size_t count) noexcept {
    std::size_t queued = 0;
    {
      const std::lock_guard lock{mutex_};
      try {
        for (; queued < count; ++queued) {
          queue_.push_back(j);
        }
      } catch (...) {  // NOLINT(bugprone-empty-catch): reported as a short count
      }
    }
    idle_.notify_all();
    return queued;
  }

 private:
  void run(std::stop_token stop) noexcept {
    for (;;) {
      job* j = nullptr;
      {
        std::unique_lock lock{mutex_};
        // The predicate ignores the stop request while work remains, so a
        // submitted job always reaches its completion: a receiver left
        // without its set_value would hang a sync_wait at process exit.
        // The stop token's job is only to wake an idle worker.
        idle_.wait(lock, stop, [this] { return !queue_.empty(); });
        if (queue_.empty()) {
          return;  // stop requested, and drained
        }
        j = queue_.front();
        queue_.pop_front();
      }
      j->work();
      if (j->release()) {
        j->complete();
        delete j;
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable_any idle_;
  std::deque<job*> queue_;
  std::vector<std::jthread> workers_;
};

struct signal_job final : job {
  repl::receiver_proxy& proxy;
  signal_job(repl::receiver_proxy& p) noexcept : job(1), proxy(p) {}
  void complete() noexcept override { proxy.set_value(); }
};

// Chunks are pulled from a shared counter rather than dealt out in fixed
// shares, so a worker that runs slower takes fewer: the same split
// the hashing engine uses one layer up.
struct bulk_job final : job {
  repl::bulk_item_receiver_proxy& proxy;
  std::size_t shape;
  std::size_t chunk_length;
  std::size_t chunks;
  std::atomic<std::size_t> next{0};

  bulk_job(std::size_t workers, repl::bulk_item_receiver_proxy& p,
           std::size_t sh, std::size_t len, std::size_t n) noexcept
      : job(workers), proxy(p), shape(sh), chunk_length(len), chunks(n) {}

  void work() noexcept override {
    for (std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
         i < chunks; i = next.fetch_add(1, std::memory_order_relaxed)) {
      const std::size_t begin = i * chunk_length;
      proxy.execute(begin, std::min(begin + chunk_length, shape));
    }
  }
  void complete() noexcept override { proxy.set_value(); }
};

class sized_backend final : public repl::parallel_scheduler_backend {
 public:
  explicit sized_backend(unsigned threads) : pool_(threads) {}

  void schedule(repl::receiver_proxy& proxy,
                std::span<std::byte>) noexcept override {
    job* j = nullptr;
    try {
      j = new signal_job(proxy);
    } catch (...) {
      proxy.set_error(std::current_exception());
      return;
    }
    if (pool_.submit(j, 1) == 0) {
      delete j;
      proxy.set_error(std::make_exception_ptr(
          std::runtime_error("blake3pp: parallel scheduler queue full")));
      return;
    }
    settle(j, 1, 1);
  }

  void schedule_bulk_chunked(std::size_t shape,
                             repl::bulk_item_receiver_proxy& proxy,
                             std::span<std::byte> storage) noexcept override {
    const std::size_t n = pool_.size();
    schedule_bulk(shape, (shape + n - 1) / n, proxy, storage);
  }

  void schedule_bulk_unchunked(std::size_t shape,
                               repl::bulk_item_receiver_proxy& proxy,
                               std::span<std::byte> storage) noexcept override {
    schedule_bulk(shape, 1, proxy, storage);
  }

 private:
  // The submitter's own guard: whoever brings the count to zero completes,
  // so a worker cannot finish and delete the job while more are queued.
  static void settle(job* j, std::size_t wanted, std::size_t queued) noexcept {
    if (j->release(wanted - queued + 1)) {
      j->complete();
      delete j;
    }
  }

  void schedule_bulk(std::size_t shape, std::size_t chunk_length,
                     repl::bulk_item_receiver_proxy& proxy,
                     std::span<std::byte> storage) noexcept {
    if (shape == 0) {
      schedule(proxy, storage);
      return;
    }
    const std::size_t chunks = (shape + chunk_length - 1) / chunk_length;
    const std::size_t wanted = std::min(chunks, pool_.size());
    bulk_job* j = nullptr;
    try {
      j = new bulk_job(wanted, proxy, shape, chunk_length, chunks);
    } catch (...) {
      proxy.set_error(std::current_exception());
      return;
    }
    const std::size_t queued = pool_.submit(j, wanted);
    if (queued == 0) {
      delete j;
      proxy.set_error(std::make_exception_ptr(
          std::runtime_error("blake3pp: parallel scheduler queue full")));
      return;
    }
    settle(j, wanted, queued);
  }

  pool pool_;
};

}  // namespace
}  // namespace blake3pp

namespace beman::execution::parallel_scheduler_replacement {

// The replacement P2079 sanctions: one definition per program, chosen at
// link time by whoever links blake3pp::parallel_backend.
auto query_parallel_scheduler_backend()
    -> std::shared_ptr<parallel_scheduler_backend> {
  static auto backend = [] {
    unsigned threads = blake3pp::parallel_scheduler_threads();
    if (threads == 0) {
      threads = std::max(1u, std::thread::hardware_concurrency());
    }
    blake3pp::g_started.store(true, std::memory_order_relaxed);
    return std::make_shared<blake3pp::sized_backend>(threads);
  }();
  return backend;
}

}  // namespace beman::execution::parallel_scheduler_replacement

#endif  // BLAKE3PP_EXECUTION_BEMAN

namespace blake3pp {

void size_parallel_scheduler(unsigned threads) {
  if (threads == 0) {
    throw std::invalid_argument(
        "blake3pp::size_parallel_scheduler: threads must be at least 1");
  }
  const std::lock_guard lock{g_mutex};
  if (scheduler_started()) {
    already("the parallel scheduler is already running");
  }
  const unsigned sized = g_threads.load(std::memory_order_relaxed);
  if (sized != 0 && sized != threads) {
    already("already sized to " + std::to_string(sized));
  }
  g_threads.store(threads, std::memory_order_relaxed);
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
  detail::process_pool_threads().store(threads, std::memory_order_relaxed);
#endif
}

unsigned parallel_scheduler_threads() noexcept {
  return g_threads.load(std::memory_order_relaxed);
}

}  // namespace blake3pp
