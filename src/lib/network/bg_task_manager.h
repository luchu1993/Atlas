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

// Abstract background task interface
class BackgroundTask {
 public:
  virtual ~BackgroundTask() = default;

  // Runs on a thread pool thread. Must NOT access network objects.
  virtual void DoBackgroundTask() = 0;

  // Runs on the main (dispatcher) thread after background work completes.
  virtual void DoMainThreadTask() = 0;
};

// Thread safety: add_task() is safe to call from any thread.
// do_main_thread_task() callbacks run on the EventDispatcher thread only.
// Must outlive all submitted tasks (destructor waits for completion).
class BgTaskManager : public FrequentTask {
 public:
  explicit BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads = 4);
  ~BgTaskManager() override;

  BgTaskManager(const BgTaskManager&) = delete;
  BgTaskManager& operator=(const BgTaskManager&) = delete;

  // Submit a background task
  void AddTask(std::unique_ptr<BackgroundTask> task);

  // Lambda convenience: bg_work runs on pool, main_callback runs on dispatcher thread
  void AddTask(std::function<void()> bg_work, std::function<void()> main_callback);

  // Query
  [[nodiscard]] auto PendingCount() const -> uint32_t;
  [[nodiscard]] auto InFlightCount() const -> uint32_t;

 private:
  // FrequentTask -- processes completed tasks on main thread
  void DoTask() override;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  // IMPORTANT: registration_ must be declared after impl_ so the destructor
  // removes the task before impl_ (which owns the dispatcher reference) is torn down.
  FrequentTaskRegistration registration_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_
