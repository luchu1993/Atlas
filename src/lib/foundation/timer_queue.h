#ifndef ATLAS_LIB_FOUNDATION_TIMER_QUEUE_H_
#define ATLAS_LIB_FOUNDATION_TIMER_QUEUE_H_

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "foundation/clock.h"
#include "foundation/pool_allocator.h"

namespace atlas {

class TimerHandle {
 public:
  constexpr TimerHandle() = default;
  [[nodiscard]] constexpr auto IsValid() const -> bool { return id_ != 0; }
  [[nodiscard]] constexpr auto Id() const -> uint64_t { return id_; }
  constexpr auto operator<=>(const TimerHandle&) const = default;

 private:
  friend class TimerQueue;
  explicit constexpr TimerHandle(uint64_t id) : id_(id) {}
  uint64_t id_{0};
};

// Thread safety: NOT thread-safe. Reentrant: callbacks may call schedule/cancel.
class TimerQueue {
 public:
  using Callback = std::function<void(TimerHandle)>;

  TimerQueue() = default;
  ~TimerQueue();

  // Non-copyable
  TimerQueue(const TimerQueue&) = delete;
  TimerQueue& operator=(const TimerQueue&) = delete;

  [[nodiscard]] auto Schedule(TimePoint when, Callback callback) -> TimerHandle;
  [[nodiscard]] auto ScheduleRepeating(TimePoint first_fire, Duration interval, Callback callback)
      -> TimerHandle;

  auto Cancel(TimerHandle handle) -> bool;
  auto Process(TimePoint now) -> uint32_t;

  [[nodiscard]] auto TimeUntilNext(TimePoint now) const -> Duration;
  [[nodiscard]] auto size() const -> uint32_t;
  [[nodiscard]] auto empty() const -> bool;

  void clear();

 private:
  struct Node {
    uint64_t id;
    TimePoint fire_time;
    Duration interval;  // zero for one-shot
    Callback callback;
    bool cancelled{false};
  };

  // Min-heap comparator: earlier fire_time has higher priority
  struct HeapCompare {
    auto operator()(const Node* a, const Node* b) const -> bool {
      return a->fire_time > b->fire_time;  // std::greater for min-heap
    }
  };

  // Keeps timer churn visible as "TimerNode" in Tracy memory views.
  TypedPool<Node> node_pool_{"TimerNode", 64};

  std::vector<Node*> heap_;
  std::unordered_map<uint64_t, Node*> index_;
  uint64_t next_id_{1};

  void PurgeCancelled();
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_TIMER_QUEUE_H_
