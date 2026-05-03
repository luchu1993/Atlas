#include "platform/threading.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

namespace atlas {

struct ThreadPool::Impl {
  // std::thread, not std::jthread — Apple Clang libc++ on iOS lacks the
  // C++20 jthread/stop_token pieces, and the worker loop never used them.
  std::vector<std::thread> workers;
  std::queue<std::function<void()>> tasks;
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> stopped{false};
  std::atomic<uint32_t> pending{0};
};

ThreadPool::ThreadPool(uint32_t num_threads) : impl_(std::make_unique<Impl>()) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
      num_threads = 1;
    }
  }

  impl_->workers.reserve(num_threads);

  for (uint32_t i = 0; i < num_threads; ++i) {
    impl_->workers.emplace_back([this]() {
      while (true) {
        std::function<void()> task;

        {
          std::unique_lock lock(impl_->mutex);
          impl_->cv.wait(lock, [this]() {
            return !impl_->tasks.empty() || impl_->stopped.load(std::memory_order_relaxed);
          });

          // Exit only when stopped AND no remaining tasks (drain queue)
          if (impl_->stopped.load(std::memory_order_relaxed) && impl_->tasks.empty()) {
            return;
          }

          if (impl_->tasks.empty()) {
            continue;
          }

          task = std::move(impl_->tasks.front());
          impl_->tasks.pop();
          impl_->pending.fetch_sub(1, std::memory_order_relaxed);
        }

        task();
      }
    });
  }
}

ThreadPool::~ThreadPool() {
  Shutdown();
}

void ThreadPool::Enqueue(std::function<void()> task) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped.load(std::memory_order_relaxed)) {
      return;  // packaged_task dtor sets broken_promise
    }
    impl_->tasks.push(std::move(task));
    impl_->pending.fetch_add(1, std::memory_order_relaxed);
  }
  impl_->cv.notify_one();
}

void ThreadPool::Shutdown() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped.exchange(true)) {
      return;  // Already shut down
    }
  }

  // Setting stopped under the mutex prevents a lost-wakeup race.
  impl_->cv.notify_all();

  // std::thread does not auto-join; do it explicitly before clearing.
  for (auto& worker : impl_->workers) {
    if (worker.joinable()) worker.join();
  }
  impl_->workers.clear();
}

auto ThreadPool::ThreadCount() const -> uint32_t {
  return static_cast<uint32_t>(impl_->workers.size());
}

auto ThreadPool::PendingTasks() const -> uint32_t {
  return impl_->pending.load(std::memory_order_relaxed);
}

}  // namespace atlas
