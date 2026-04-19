#ifndef ATLAS_LIB_SPACE_MOVE_CONTROLLER_H_
#define ATLAS_LIB_SPACE_MOVE_CONTROLLER_H_

#include "math/vector3.h"
#include "space/controller.h"

namespace atlas {

// ============================================================================
// MoveToPointController — linear interpolation towards a destination
//
// Each tick the entity's position advances by min(speed*dt, remaining) in
// the direction of the destination. On arrival the controller marks
// Finish() so Controllers evicts it next tick.
//
// `face_movement`: when true, the entity's forward direction is rotated to
// look along the motion vector; when false, direction is left alone (useful
// for e.g. strafing NPCs). The actual direction mutation lives in CellEntity
// so this module stays position-only; a Step 10.4 wiring plumbs it through.
// ============================================================================

class MoveToPointController final : public Controller {
 public:
  MoveToPointController(const math::Vector3& destination, float speed, bool face_movement)
      : destination_(destination), speed_(speed), face_movement_(face_movement) {}

  [[nodiscard]] auto Destination() const -> const math::Vector3& { return destination_; }
  [[nodiscard]] auto Speed() const -> float { return speed_; }
  [[nodiscard]] auto FaceMovement() const -> bool { return face_movement_; }

  void Update(float dt) override;

 private:
  math::Vector3 destination_;
  float speed_;
  bool face_movement_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_MOVE_CONTROLLER_H_
