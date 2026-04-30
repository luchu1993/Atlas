#ifndef ATLAS_LIB_SPACE_ENTITY_MOTION_H_
#define ATLAS_LIB_SPACE_ENTITY_MOTION_H_

#include "math/vector3.h"

namespace atlas {

class IEntityMotion {
 public:
  virtual ~IEntityMotion() = default;

  [[nodiscard]] virtual auto Position() const -> const math::Vector3& = 0;
  virtual void SetPosition(const math::Vector3& pos) = 0;

  [[nodiscard]] virtual auto Direction() const -> const math::Vector3& = 0;
  virtual void SetDirection(const math::Vector3& dir) = 0;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_ENTITY_MOTION_H_
