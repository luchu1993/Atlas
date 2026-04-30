#include "network/bg_task_manager.h"

#include "foundation/callback_utils.h"
#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "platform/threading.h"

namespace atlas {

class LambdaTask : public BackgroundTask {
 public:
  LambdaTask(std::function<void()> bg, std::function<void()> main)
      : bg_work_(std::move(bg)), main_callback_(std::move(main)) {}

  void DoBackgroundTask() override { bg_work_(); }
  void DoMainThreadTask() override { main_callback_(); }

 private:
  std::function<void()> bg_work_;
  std::function<void()> main_callback_;
};

struct BgTaskManager::Impl {
  EventDispatcher& dispatcher;
  ThreadPool pool;
  std::mutex completed_mutex;
  std::vector<std::unique_ptr<BackgroundTask>> completed;
  std::atomic<uint32_t> in_flight{0};

  Impl(EventDispatcher& d, uint32_t threads) : dispatcher(d), pool(threads) {}
};

BgTaskManager::BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads)
    : impl_(std::make_unique<Impl>(dispatcher, num_threads)),
      registration_(impl_->dispatcher.AddFrequentTask(this)) {}

BgTaskManager::~BgTaskManager() {
  registration_.Reset();
  impl_->pool.Shutdown();
  DoTask();
}

void BgTaskManager::AddTask(std::unique_ptr<BackgroundTask> task) {
  impl_->in_flight.fetch_add(1, std::memory_order_relaxed);

  auto* raw = task.release();
  impl_->pool.Submit([this, raw]() {
    std::unique_ptr<BackgroundTask> owned(raw);
    SafeInvoke("BackgroundTask::do_background_task", [&] { owned->DoBackgroundTask(); });
    {
      std::lock_guard lock(impl_->completed_mutex);
      impl_->completed.push_back(std::move(owned));
    }
  });
}

void BgTaskManager::AddTask(std::function<void()> bg_work, std::function<void()> main_callback) {
  AddTask(std::make_unique<LambdaTask>(std::move(bg_work), std::move(main_callback)));
}

auto BgTaskManager::PendingCount() const -> uint32_t {
  return impl_->pool.PendingTasks();
}

auto BgTaskManager::InFlightCount() const -> uint32_t {
  return impl_->in_flight.load(std::memory_order_acquire);
}

void BgTaskManager::DoTask() {
  std::vector<std::unique_ptr<BackgroundTask>> tasks;
  {
    std::lock_guard lock(impl_->completed_mutex);
    tasks.swap(impl_->completed);
  }

  for (auto& task : tasks) {
    SafeInvoke("BackgroundTask::do_main_thread_task", [&] { task->DoMainThreadTask(); });
    impl_->in_flight.fetch_sub(1, std::memory_order_release);
  }
}

}  // namespace atlas
