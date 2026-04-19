#ifndef ATLAS_LIB_SPACE_TIMER_CONTROLLER_H_
#define ATLAS_LIB_SPACE_TIMER_CONTROLLER_H_

#include <functional>

#include "space/controller.h"

namespace atlas {

// ============================================================================
// TimerController — fire a callback after `interval` seconds
//
// Two modes:
//   - One-shot (repeat=false): Finish() is called immediately after the
//     first invocation; Controllers evicts us next tick.
//   - Repeating (repeat=true): we subtract `interval` from the accumulator
//     and fire again. We intentionally do NOT reset accumulator to zero —
//     any overshoot caused by a large dt carries over so the average rate
//     over time matches 1/interval.
//
// The `on_fire` callback sees the owning `IEntityMotion*` (may be nullptr
// — Timer often ticks without any motion surface) and the controller's
// `user_arg`. Callbacks can cancel themselves via Controllers::Cancel;
// reentrancy is safe because Cancel defers (see controllers.h).
// ============================================================================

class TimerController final : public Controller {
 public:
  using FireFn = std::function<void(TimerController& self)>;

  TimerController(float interval_seconds, bool repeat, FireFn on_fire = nullptr)
      : interval_(interval_seconds), repeat_(repeat), on_fire_(std::move(on_fire)) {}

  [[nodiscard]] auto Interval() const -> float { return interval_; }
  [[nodiscard]] auto Repeat() const -> bool { return repeat_; }

  // The running accumulator is read by tests to assert timing behaviour;
  // it's a tick-local detail and not meaningful to game code.
  [[nodiscard]] auto Accumulated() const -> float { return accumulated_; }
  [[nodiscard]] auto FireCount() const -> uint32_t { return fire_count_; }

  void Update(float dt) override;

 private:
  float interval_;
  bool repeat_;
  FireFn on_fire_;

  float accumulated_{0.f};
  uint32_t fire_count_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_TIMER_CONTROLLER_H_
