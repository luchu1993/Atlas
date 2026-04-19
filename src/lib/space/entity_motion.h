#ifndef ATLAS_LIB_SPACE_ENTITY_MOTION_H_
#define ATLAS_LIB_SPACE_ENTITY_MOTION_H_

#include "math/vector3.h"

namespace atlas {

// ============================================================================
// IEntityMotion — minimal motion-control surface consumed by controllers
//
// Declared here in src/lib/space so the space library (RangeList, Range-
// Trigger, Controller) stays independent of server/cellapp. CellEntity
// (Step 10.4) implements this interface; tests stub it with a tiny mock.
//
// Every controller that physically moves an entity (MoveToPointController,
// future navigation/steering controllers) goes through this surface. Pure
// time-based controllers (Timer) don't need it and can ignore the entity
// pointer entirely.
// ============================================================================

class IEntityMotion {
 public:
  virtual ~IEntityMotion() = default;

  [[nodiscard]] virtual auto Position() const -> const math::Vector3& = 0;
  virtual void SetPosition(const math::Vector3& pos) = 0;

  // Direction is the entity's facing vector. Call conventions: unit
  // length, world-space. Controllers that rotate the entity (e.g.
  // MoveToPointController with face_movement=true) write here; others
  // leave it alone.
  [[nodiscard]] virtual auto Direction() const -> const math::Vector3& = 0;
  virtual void SetDirection(const math::Vector3& dir) = 0;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_ENTITY_MOTION_H_
