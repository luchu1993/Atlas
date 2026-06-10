#ifndef ATLAS_LIB_SPACE_MOVE_ALONG_PATH_CONTROLLER_H_
#define ATLAS_LIB_SPACE_MOVE_ALONG_PATH_CONTROLLER_H_

#include <cstddef>
#include <utility>
#include <vector>

#include "math/vector3.h"
#include "space/controller.h"

namespace atlas {

// Walks a pre-planned polyline (e.g. a navmesh path). Planning happens at the
// call site, so this stays valid across a Space navmesh reload and its state
// serializes for offload migration.
class MoveAlongPathController final : public Controller {
 public:
  MoveAlongPathController(std::vector<math::Vector3> waypoints, float speed, bool face_movement,
                          std::size_t next_index = 0)
      : waypoints_(std::move(waypoints)),
        speed_(speed),
        face_movement_(face_movement),
        next_index_(next_index) {}

  [[nodiscard]] auto Waypoints() const -> const std::vector<math::Vector3>& { return waypoints_; }
  [[nodiscard]] auto Speed() const -> float { return speed_; }
  [[nodiscard]] auto FaceMovement() const -> bool { return face_movement_; }
  [[nodiscard]] auto NextIndex() const -> std::size_t { return next_index_; }

  void Update(float dt) override;
  [[nodiscard]] auto TypeTag() const -> ControllerKind override {
    return ControllerKind::kMoveAlongPath;
  }

 private:
  std::vector<math::Vector3> waypoints_;
  float speed_;
  bool face_movement_;
  std::size_t next_index_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_MOVE_ALONG_PATH_CONTROLLER_H_
