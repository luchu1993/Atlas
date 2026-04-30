#ifndef ATLAS_LIB_SPACE_TIMER_CONTROLLER_H_
#define ATLAS_LIB_SPACE_TIMER_CONTROLLER_H_

#include <functional>

#include "space/controller.h"

namespace atlas {

class TimerController final : public Controller {
 public:
  using FireFn = std::function<void(TimerController& self)>;

  TimerController(float interval_seconds, bool repeat, FireFn on_fire = nullptr)
      : interval_(interval_seconds), repeat_(repeat), on_fire_(std::move(on_fire)) {}

  [[nodiscard]] auto Interval() const -> float { return interval_; }
  [[nodiscard]] auto Repeat() const -> bool { return repeat_; }

  [[nodiscard]] auto Accumulated() const -> float { return accumulated_; }
  [[nodiscard]] auto FireCount() const -> uint32_t { return fire_count_; }

  void Update(float dt) override;
  [[nodiscard]] auto TypeTag() const -> ControllerKind override { return ControllerKind::kTimer; }

  // Migration restore; valid only before Start() on the receiving CellApp.
  void RestoreRunningStateForMigration(float accumulated, uint32_t fire_count) {
    accumulated_ = accumulated;
    fire_count_ = fire_count;
  }

 private:
  float interval_;
  bool repeat_;
  FireFn on_fire_;

  float accumulated_{0.f};
  uint32_t fire_count_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_TIMER_CONTROLLER_H_
