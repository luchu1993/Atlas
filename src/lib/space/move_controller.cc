#include "space/move_controller.h"

#include <cassert>
#include <cmath>

#include "space/entity_motion.h"

namespace atlas {

void MoveToPointController::Update(float dt) {
  assert(Motion() != nullptr && "MoveToPointController requires an IEntityMotion");
  auto* motion = Motion();

  const math::Vector3 current = motion->Position();
  const math::Vector3 delta = destination_ - current;
  const float dist = delta.Length();

  // Target reached (or arbitrarily close) — snap to destination, optionally
  // update facing one last time, and mark done.
  const float step = speed_ * dt;
  if (dist <= step || dist < 1e-4f) {
    motion->SetPosition(destination_);
    if (face_movement_ && dist > 1e-4f) {
      // Face the final approach direction, not whatever stale direction
      // was stored. Use the normalised residual vector.
      motion->SetDirection(delta * (1.0f / dist));
    }
    Finish();
    return;
  }

  // Still in transit: advance `step` along the delta direction.
  const float inv = 1.0f / dist;
  const math::Vector3 unit = delta * inv;
  motion->SetPosition(current + unit * step);
  if (face_movement_) motion->SetDirection(unit);
}

}  // namespace atlas
