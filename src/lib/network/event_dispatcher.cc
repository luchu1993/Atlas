#include "network/event_dispatcher.h"

#include <algorithm>

#include "foundation/log.h"

namespace atlas {

EventDispatcher::EventDispatcher(std::string_view name)
    : name_(name), poller_(IOPoller::Create()) {}

EventDispatcher::~EventDispatcher() = default;

auto EventDispatcher::RegisterReader(FdHandle fd, IOCallback callback) -> Result<void> {
  return poller_->Add(fd, IOEvent::kReadable, std::move(callback));
}

auto EventDispatcher::RegisterWriter(FdHandle fd, IOCallback callback) -> Result<void> {
  return poller_->Add(fd, IOEvent::kWritable, std::move(callback));
}

auto EventDispatcher::ModifyInterest(FdHandle fd, IOEvent interest) -> Result<void> {
  return poller_->Modify(fd, interest);
}

auto EventDispatcher::Deregister(FdHandle fd) -> Result<void> {
  // Safe during poll dispatch: IOPoller implementations swap the
  // callback out for invocation and re-resolve the entry when the map changes.
  return poller_->Remove(fd);
}

auto EventDispatcher::AddTimer(Duration delay, TimerQueue::Callback callback) -> TimerHandle {
  return timers_.Schedule(Clock::now() + delay, std::move(callback));
}

auto EventDispatcher::AddRepeatingTimer(Duration interval, TimerQueue::Callback callback)
    -> TimerHandle {
  return timers_.ScheduleRepeating(Clock::now() + interval, interval, std::move(callback));
}

auto EventDispatcher::CancelTimer(TimerHandle handle) -> bool {
  return timers_.Cancel(handle);
}

FrequentTaskRegistration::~FrequentTaskRegistration() {
  Reset();
}

void FrequentTaskRegistration::Reset() {
  if (dispatcher_ && task_) {
    dispatcher_->RemoveFrequentTask(task_);
    dispatcher_ = nullptr;
    task_ = nullptr;
  }
}

auto EventDispatcher::AddFrequentTask(FrequentTask* task) -> FrequentTaskRegistration {
  auto it = std::find(tasks_.begin(), tasks_.end(), task);
  if (it == tasks_.end()) {
    tasks_.push_back(task);
  }
  return FrequentTaskRegistration(this, task);
}

void EventDispatcher::RemoveFrequentTask(FrequentTask* task) {
  if (iterating_tasks_) {
    auto it = std::find(tasks_.begin(), tasks_.end(), task);
    if (it != tasks_.end()) {
      *it = nullptr;
      tasks_dirty_ = true;
    }
  } else {
    std::erase(tasks_, task);
  }
}

void EventDispatcher::ProcessFrequentTasks() {
  iterating_tasks_ = true;
  for (auto* task : tasks_) {
    if (task != nullptr) {
      task->DoTask();
    }
  }
  iterating_tasks_ = false;

  if (tasks_dirty_) {
    std::erase(tasks_, nullptr);
    tasks_dirty_ = false;
  }
}

void EventDispatcher::ProcessOnce() {
  ProcessFrequentTasks();

  auto now = Clock::now();
  timers_.Process(now);

  auto time_to_timer = timers_.TimeUntilNext(now);
  auto timeout = std::min({time_to_timer, max_poll_wait_, adaptive_poll_wait_});
  if (timeout < kMinPollWait) {
    timeout = kMinPollWait;
  }

  auto result = poller_->Poll(timeout);
  if (!result) {
    ATLAS_LOG_ERROR("EventDispatcher poll error: {}", result.Error().Message());
  }

  if (result && *result > 0) {
    adaptive_poll_wait_ = kMinPollWait;
  } else {
    adaptive_poll_wait_ = std::min(adaptive_poll_wait_ * 2, kMaxAdaptivePollWait);
  }

  timers_.Process(Clock::now());
}

void EventDispatcher::Run() {
  running_ = true;
  stop_requested_ = false;
  while (!stop_requested_) {
    ProcessOnce();
  }
  running_ = false;
}

void EventDispatcher::Stop() {
  stop_requested_ = true;
}

void EventDispatcher::SetMaxPollWait(Duration max_wait) {
  max_poll_wait_ = max_wait;
}

}  // namespace atlas
