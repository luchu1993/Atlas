#ifndef ATLAS_LIB_PLATFORM_THREADING_H_
#define ATLAS_LIB_PLATFORM_THREADING_H_

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>
#include <thread>

#include "platform/platform_config.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace atlas {

// ============================================================================
// Thread naming
// ============================================================================

void SetThreadName(std::string_view name);
void SetThreadName(std::jthread& thread, std::string_view name);

// ============================================================================
// SpinLock -- for very short critical sections
// ============================================================================

class SpinLock {
 public:
  void Lock() noexcept {
    while (flag_.test_and_set(std::memory_order_acquire)) {
      // Inner loop re-reads without a costly test_and_set until the flag
      // looks free, then retries the atomic exchange.  The pause hint
      // reduces pipeline pressure and improves hyper-thread throughput.
      while (flag_.test(std::memory_order_relaxed)) {
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
      }
    }
  }

  void Unlock() noexcept { flag_.clear(std::memory_order_release); }

  [[nodiscard]] auto TryLock() noexcept -> bool {
    return !flag_.test_and_set(std::memory_order_acquire);
  }

  // STL Lockable interface aliases (BasicLockable / Lockable named requirements)
  void lock() noexcept { Lock(); }
  void unlock() noexcept { Unlock(); }
  [[nodiscard]] auto try_lock() noexcept -> bool { return TryLock(); }

 private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ============================================================================
// ThreadPool
// ============================================================================

class ThreadPool {
 public:
  explicit ThreadPool(uint32_t num_threads = 0);  // 0 = hardware_concurrency
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto Submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using R = std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<R()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    auto future = task->get_future();
    Enqueue([task = std::move(task)]() { (*task)(); });
    return future;
  }

  [[nodiscard]] auto ThreadCount() const -> uint32_t;
  [[nodiscard]] auto PendingTasks() const -> uint32_t;

  void Shutdown();

 private:
  void Enqueue(std::function<void()> task);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_THREADING_H_
