#ifndef ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_
#define ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "network/event_dispatcher.h"
#include "network/frequent_task.h"

namespace atlas {

class EventDispatcher;

class BackgroundTask {
 public:
  virtual ~BackgroundTask() = default;

  // Must not access dispatcher-owned network objects.
  virtual void DoBackgroundTask() = 0;

  virtual void DoMainThreadTask() = 0;
};

// AddTask is thread-safe; main callbacks run on the dispatcher thread.
class BgTaskManager : public FrequentTask {
 public:
  explicit BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads = 4);
  ~BgTaskManager() override;

  BgTaskManager(const BgTaskManager&) = delete;
  BgTaskManager& operator=(const BgTaskManager&) = delete;

  void AddTask(std::unique_ptr<BackgroundTask> task);

  void AddTask(std::function<void()> bg_work, std::function<void()> main_callback);

  [[nodiscard]] auto PendingCount() const -> uint32_t;
  [[nodiscard]] auto InFlightCount() const -> uint32_t;

 private:
  void DoTask() override;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  FrequentTaskRegistration registration_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_
