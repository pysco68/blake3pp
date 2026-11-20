#pragma once

// A wake one thread leaves and another sleeps on: the library's one
// condition-variable waiter. A wake with nobody asleep is remembered, so
// the next sleep returns at once and a wake that races a sleep is not
// lost. take() consumes a remembered wake without sleeping.
//
// queued_reads parks its driver on one, the I/O layer's poll_waiter is one
// wherever it needs no descriptor (src/io/waiter.hpp), and the test
// drivers park on one.
//
// Internal to the library. Standard library only.

#include <condition_variable>
#include <mutex>
#include <utility>

namespace blake3pp::detail {

class waiter {
 public:
  waiter() = default;
  waiter(const waiter&) = delete;
  waiter& operator=(const waiter&) = delete;

  void wake() noexcept {
    {
      const std::lock_guard<std::mutex> lock(m_);
      signalled_ = true;
    }
    cv_.notify_one();
  }

  [[nodiscard]] bool take() noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return std::exchange(signalled_, false);
  }

  void sleep() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [this] { return signalled_; });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool signalled_ = false;
};

}  // namespace blake3pp::detail
