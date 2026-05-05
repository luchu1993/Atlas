#ifndef ATLAS_LIB_NETWORK_EVENT_DISPATCHER_H_
#define ATLAS_LIB_NETWORK_EVENT_DISPATCHER_H_

#include <memory>
#include <string_view>
#include <vector>

#include "foundation/clock.h"
#include "foundation/timer_queue.h"
#include "network/frequent_task.h"
#include "platform/io_poller.h"

namespace atlas {

class EventDispatcher;

// Must be destroyed before the referenced EventDispatcher.
class FrequentTaskRegistration {
 public:
  FrequentTaskRegistration() = default;

  ~FrequentTaskRegistration();

  FrequentTaskRegistration(const FrequentTaskRegistration&) = delete;
  FrequentTaskRegistration& operator=(const FrequentTaskRegistration&) = delete;

  FrequentTaskRegistration(FrequentTaskRegistration&& other) noexcept
      : dispatcher_(other.dispatcher_), task_(other.task_) {
    other.dispatcher_ = nullptr;
    other.task_ = nullptr;
  }

  FrequentTaskRegistration& operator=(FrequentTaskRegistration&& other) noexcept {
    if (this != &other) {
      Reset();
      dispatcher_ = other.dispatcher_;
      task_ = other.task_;
      other.dispatcher_ = nullptr;
      other.task_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] auto IsValid() const -> bool { return dispatcher_ != nullptr; }

  void Reset();

 private:
  friend class EventDispatcher;
  FrequentTaskRegistration(EventDispatcher* d, FrequentTask* t) : dispatcher_(d), task_(t) {}

  EventDispatcher* dispatcher_{nullptr};
  FrequentTask* task_{nullptr};
};

// Not thread-safe; callbacks may mutate dispatcher registrations.
class EventDispatcher {
 public:
  explicit EventDispatcher(std::string_view name = "main");
  ~EventDispatcher();

  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;

  [[nodiscard]] auto RegisterReader(FdHandle fd, IOCallback callback) -> Result<void>;
  [[nodiscard]] auto RegisterWriter(FdHandle fd, IOCallback callback) -> Result<void>;
  [[nodiscard]] auto ModifyInterest(FdHandle fd, IOEvent interest) -> Result<void>;
  [[nodiscard]] auto Deregister(FdHandle fd) -> Result<void>;

  [[nodiscard]] auto AddTimer(Duration delay, TimerQueue::Callback callback) -> TimerHandle;
  [[nodiscard]] auto AddRepeatingTimer(Duration interval, TimerQueue::Callback callback)
      -> TimerHandle;
  auto CancelTimer(TimerHandle handle) -> bool;

  [[nodiscard]] auto AddFrequentTask(FrequentTask* task) -> FrequentTaskRegistration;

  void RemoveFrequentTask(FrequentTask* task);

  void ProcessOnce();
  void Run();
  void Stop();
  [[nodiscard]] auto IsRunning() const -> bool { return running_; }

  void SetMaxPollWait(Duration max_wait);

  [[nodiscard]] auto Name() const -> std::string_view { return name_; }

 private:
  void ProcessFrequentTasks();

  std::string name_;
  std::unique_ptr<IOPoller> poller_;
  TimerQueue timers_;
  std::vector<FrequentTask*> tasks_;
  bool iterating_tasks_{false};
  bool tasks_dirty_{false};

  bool running_{false};
  bool stop_requested_{false};
  Duration max_poll_wait_{Milliseconds(100)};
  Duration adaptive_poll_wait_{Milliseconds(1)};
  // 1ms floor matches what epoll/WSAPoll can express; sub-ms truncates to 0
  // (non-blocking) and races with packets in flight on the same loop iteration.
  static constexpr Duration kMinPollWait{Milliseconds(1)};
  static constexpr Duration kMaxAdaptivePollWait{Milliseconds(10)};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_EVENT_DISPATCHER_H_
