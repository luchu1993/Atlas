#ifndef ATLAS_UE_CLIENT_CORE_MOVING_CLIENT_ENTITY_H_
#define ATLAS_UE_CLIENT_CORE_MOVING_CLIENT_ENTITY_H_

#include "AtlasCore/avatar_filter.h"
#include "AtlasCore/client_entity.h"
#include "AtlasCore/entity_view.h"

namespace atlas {

// Generic spatial-entity adapter: feeds position updates through
// AvatarFilter and pushes interpolated transforms out via the attached
// view. Any entity that needs jitter-smoothed motion (Avatar, Npc,
// Projectile, …) can use this directly — per-type subclasses only need
// to override when they introduce extra runtime state.
class MovingClientEntity : public ClientEntity {
 public:
  MovingClientEntity(EntityId id, EntityTypeId type_id) : ClientEntity(id, type_id) {}

  void OnPositionReceived(double server_time, const Vec3& pos, const Vec3& dir,
                          bool on_ground) override {
    filter_.Input(server_time, pos, dir, on_ground);
  }

  void TickInterpolation(double dt) override {
    EntityView* v = View();
    if (v == nullptr) return;
    filter_.UpdateLatency(dt);
    Vec3 pos;
    Vec3 dir;
    bool on_ground = false;
    if (filter_.TryEvaluate(pos, dir, on_ground)) {
      v->OnTransformReplicated(pos, Quat{});
    }
  }

  [[nodiscard]] const AvatarFilter& Filter() const { return filter_; }

 private:
  AvatarFilter filter_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_MOVING_CLIENT_ENTITY_H_
