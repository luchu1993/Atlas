#ifndef ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_CURVE_STORE_H_
#define ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_CURVE_STORE_H_

#include <cstddef>
#include <unordered_map>

#include "movement_sim/movement_sim.h"

namespace atlas::movement {

class MovementCurveStore {
 public:
  [[nodiscard]] auto Set(const MovementCurve& curve) -> bool;
  [[nodiscard]] auto Find(uint16_t curve_id) const -> const MovementCurve*;
  void Erase(uint16_t curve_id);

  [[nodiscard]] auto Size() const -> std::size_t { return curves_.size(); }

 private:
  std::unordered_map<uint16_t, MovementCurve> curves_;
};

[[nodiscard]] auto MakeLinearMovementCurve(uint16_t curve_id) -> MovementCurve;

}  // namespace atlas::movement

#endif  // ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_CURVE_STORE_H_
