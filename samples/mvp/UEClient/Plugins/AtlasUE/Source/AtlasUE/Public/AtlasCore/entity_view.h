#ifndef ATLAS_UE_CLIENT_CORE_ENTITY_VIEW_H_
#define ATLAS_UE_CLIENT_CORE_ENTITY_VIEW_H_

#include <cstdint>

namespace atlas {

struct Vec3 {
  float x{};
  float y{};
  float z{};
};

struct Quat {
  float x{};
  float y{};
  float z{};
  float w{1.0f};
};

// Renderer / UI binding for one entity. Owned by the entity, notified on each
// replicated state change so the engine layer can update its proxy actor.
class EntityView {
 public:
  virtual ~EntityView() = default;

  virtual void OnTransformReplicated(const Vec3& position, const Quat& rotation) = 0;
  virtual void OnPropertyChanged(uint16_t field_id) = 0;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_ENTITY_VIEW_H_
