#include "movement_sim/movement_curve_store.h"

namespace atlas::movement {

auto MovementCurveStore::Set(const MovementCurve& curve) -> bool {
  if (!IsMovementCurveValid(curve)) return false;
  curves_[curve.id] = curve;
  return true;
}

auto MovementCurveStore::Find(uint16_t curve_id) const -> const MovementCurve* {
  auto it = curves_.find(curve_id);
  return it == curves_.end() ? nullptr : &it->second;
}

void MovementCurveStore::Erase(uint16_t curve_id) {
  curves_.erase(curve_id);
}

auto MakeLinearMovementCurve(uint16_t curve_id) -> MovementCurve {
  MovementCurve curve;
  curve.id = curve_id;
  curve.sample_count = 2;
  curve.samples[0] = 0.0f;
  curve.samples[1] = 1.0f;
  return curve;
}

}  // namespace atlas::movement
