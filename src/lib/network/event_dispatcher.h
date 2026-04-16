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

// ============================================================================
// FrequentTaskRegistration — RAII token returned by add_frequent_task()
// ============================================================================
//
// Destructor automatically calls remove_frequent_task() on the dispatcher.
// Store as a member in objects that register a FrequentTask — they will be
// deregistered when the owner is destroyed, with no manual remove_frequent_task()
// calls needed in the destructor.
//
// IMPORTANT: The registration must be destroyed before the referenced
// EventDispatcher.  Declare it as a member AFTER any dispatcher reference so
// that member destruction order (reverse of declaration) is safe.

class FrequentTaskRegistration {
 public:
  FrequentTaskRegistration() = default;

  ~FrequentTaskRegistration();

  // Move-only: copying would lead to double-removal.
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

  // Explicitly release the registration early (calls remove_frequent_task).
  void Reset();

 private:
  friend class EventDispatcher;
  FrequentTaskRegistration(EventDispatcher* d, FrequentTask* t) : dispatcher_(d), task_(t) {}

  EventDispatcher* dispatcher_{nullptr};
  FrequentTask* task_{nullptr};
};

// Thread safety: NOT thread-safe. All calls must originate from the same thread.
// Callbacks may safely call register/deregister/add_timer/cancel_timer/stop.
class EventDispatcher {
 public:
  explicit EventDispatcher(std::string_view name = "main");
  ~EventDispatcher();

  // Non-copyable, non-movable
  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;

  // IO registration
  [[nodiscard]] auto RegisterReader(FdHandle fd, IOCallback callback) -> Result<void>;
  [[nodiscard]] auto RegisterWriter(FdHandle fd, IOCallback callback) -> Result<void>;
  [[nodiscard]] auto ModifyInterest(FdHandle fd, IOEvent interest) -> Result<void>;
  [[nodiscard]] auto Deregister(FdHandle fd) -> Result<void>;

  // Timer registration
  [[nodiscard]] auto AddTimer(Duration delay, TimerQueue::Callback callback) -> TimerHandle;
  [[nodiscard]] auto AddRepeatingTimer(Duration interval, TimerQueue::Callback callback)
      -> TimerHandle;
  auto CancelTimer(TimerHandle handle) -> bool;

  // Frequent tasks — returns a RAII token that deregisters on destruction.
  // Store the returned FrequentTaskRegistration as a member (declared after
  // any EventDispatcher reference) to guarantee automatic cleanup.
  [[nodiscard]] auto AddFrequentTask(FrequentTask* task) -> FrequentTaskRegistration;

  // Internal removal — called by FrequentTaskRegistration::reset().
  // May also be called directly when manual lifetime control is needed.
  void RemoveFrequentTask(FrequentTask* task);

  // Main loop
  void ProcessOnce();
  void Run();
  void Stop();
  [[nodiscard]] auto IsRunning() const -> bool { return running_; }

  // Configuration
  void SetMaxPollWait(Duration max_wait);

  // Statistics
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
  static constexpr Duration kMinPollWait{Microseconds(100)};
  static constexpr Duration kMaxAdaptivePollWait{Milliseconds(10)};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_EVENT_DISPATCHER_H_
