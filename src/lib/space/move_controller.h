#ifndef ATLAS_LIB_SPACE_MOVE_CONTROLLER_H_
#define ATLAS_LIB_SPACE_MOVE_CONTROLLER_H_

#include "math/vector3.h"
#include "space/controller.h"

namespace atlas {

class MoveToPointController final : public Controller {
 public:
  MoveToPointController(const math::Vector3& destination, float speed, bool face_movement)
      : destination_(destination), speed_(speed), face_movement_(face_movement) {}

  [[nodiscard]] auto Destination() const -> const math::Vector3& { return destination_; }
  [[nodiscard]] auto Speed() const -> float { return speed_; }
  [[nodiscard]] auto FaceMovement() const -> bool { return face_movement_; }

  void Update(float dt) override;
  [[nodiscard]] auto TypeTag() const -> ControllerKind override {
    return ControllerKind::kMoveToPoint;
  }

 private:
  math::Vector3 destination_;
  float speed_;
  bool face_movement_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_MOVE_CONTROLLER_H_
