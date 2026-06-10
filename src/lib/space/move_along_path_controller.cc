#include "space/move_along_path_controller.h"

#include <cassert>

#include "space/entity_motion.h"

namespace atlas {

void MoveAlongPathController::Update(float dt) {
  assert(Motion() != nullptr && "MoveAlongPathController requires an IEntityMotion");
  auto* motion = Motion();

  // Budgeted walk: a long tick may consume several short segments.
  float budget = speed_ * dt;
  math::Vector3 current = motion->Position();
  while (next_index_ < waypoints_.size()) {
    const math::Vector3& target = waypoints_[next_index_];
    const math::Vector3 delta = target - current;
    const float dist = delta.Length();
    if (dist <= budget || dist < 1e-4f) {
      current = target;
      budget -= dist;
      ++next_index_;
      continue;
    }
    const math::Vector3 unit = delta * (1.0f / dist);
    current += unit * budget;
    motion->SetPosition(current);
    if (face_movement_) motion->SetDirection(unit);
    return;
  }

  motion->SetPosition(current);
  if (face_movement_ && waypoints_.size() >= 2) {
    const auto& last = waypoints_.back();
    const auto& prev = waypoints_[waypoints_.size() - 2];
    const math::Vector3 dir = last - prev;
    const float len = dir.Length();
    if (len > 1e-4f) motion->SetDirection(dir * (1.0f / len));
  }
  Finish();
}

}  // namespace atlas
