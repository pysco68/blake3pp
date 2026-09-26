#pragma once

// Two schedulers the pipeline suites need and the providers do not
// supply: one that runs work where it was started, and one with exactly
// one agent. Both are cases the pipelines must not deadlock on, and more
// than one test file uses them.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/parallel.hpp>

namespace b3test {

// The one-thread pool parks work on nodes of its own, the same intrusive
// shape the driver's run queue uses.
struct run_node {
  run_node* next = nullptr;
  void (*run)(run_node*) noexcept = nullptr;
};

namespace ex = blake3pp::ex;
namespace compat = blake3pp::detail::ex_compat;

// A scheduler that runs the work where it was started: the case that
// must not deadlock, since the compress stage then runs inside the
// driver's own poll.
struct inline_scheduler;

template <class Receiver>
struct inline_op {
  using operation_state_concept = compat::operation_state_tag;
  inline_op(Receiver r) : rcvr(std::move(r)) {}
  Receiver rcvr;
  void start() & noexcept { ex::set_value(std::move(rcvr)); }
};

struct inline_env {
  [[nodiscard]] inline_scheduler query(
      ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept;
};

struct inline_sender {
  using sender_concept = compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t());
  template <class Receiver>
  inline_op<Receiver> connect(Receiver r) const {
    return {std::move(r)};
  }
  [[nodiscard]] inline_env get_env() const noexcept { return {}; }
};

struct inline_scheduler : compat::weakly_parallel_scheduler {
  using scheduler_concept = compat::scheduler_tag;
  [[nodiscard]] inline_sender schedule() const noexcept { return {}; }
  bool operator==(const inline_scheduler&) const noexcept = default;
};

inline inline_scheduler inline_env::query(
    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
  return {};
}

static_assert(ex::scheduler<inline_scheduler>);

// One worker thread and a queue: the other end of the range, where every
// part of every window is serialized behind a single agent.
class one_thread_pool {
 public:
  // With a delay, the worker sleeps that long before running each node:
  // a window handed to the pool then stays there long enough for the
  // driver to fail underneath it.
  explicit one_thread_pool(std::chrono::milliseconds delay = {})
      : delay_(delay), worker_([this] { run(); }) {}
  ~one_thread_pool() {
    {
      const std::lock_guard lock(m_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  one_thread_pool(const one_thread_pool&) = delete;
  one_thread_pool& operator=(const one_thread_pool&) = delete;

  void post(run_node* n) {
    {
      const std::lock_guard lock(m_);
      queue_.push_back(n);
    }
    cv_.notify_one();
  }

  struct scheduler;

  template <class Receiver>
  struct op : run_node {
    using operation_state_concept = compat::operation_state_tag;
    op(Receiver r, one_thread_pool* p) : rcvr(std::move(r)), pool(p) {
      run = [](run_node* self) noexcept {
        ex::set_value(std::move(static_cast<op*>(self)->rcvr));
      };
    }
    Receiver rcvr;
    one_thread_pool* pool;
    void start() & noexcept { pool->post(this); }
  };

  struct env {
    one_thread_pool* pool;
    [[nodiscard]] scheduler query(
        ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept;
  };

  struct sender {
    using sender_concept = compat::sender_tag;
    BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t());
    one_thread_pool* pool;
    template <class Receiver>
    op<Receiver> connect(Receiver r) const {
      return {std::move(r), pool};
    }
    [[nodiscard]] env get_env() const noexcept { return {pool}; }
  };

  struct scheduler : compat::weakly_parallel_scheduler {
    using scheduler_concept = compat::scheduler_tag;
    one_thread_pool* pool = nullptr;
    [[nodiscard]] sender schedule() const noexcept { return {pool}; }
    bool operator==(const scheduler&) const noexcept = default;
  };

  [[nodiscard]] scheduler get_scheduler() noexcept { return {{}, this}; }

 private:
  void run() {
    for (;;) {
      run_node* n = nullptr;
      {
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stop_) {
            return;
          }
          continue;
        }
        n = queue_.front();
        queue_.erase(queue_.begin());
      }
      if (delay_.count() > 0) {
        std::this_thread::sleep_for(delay_);
      }
      n->run(n);
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::vector<run_node*> queue_;
  bool stop_ = false;
  std::chrono::milliseconds delay_;
  std::thread worker_;
};

inline one_thread_pool::scheduler one_thread_pool::env::query(
    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
  return {{}, pool};
}

static_assert(ex::scheduler<one_thread_pool::scheduler>);

}  // namespace b3test
