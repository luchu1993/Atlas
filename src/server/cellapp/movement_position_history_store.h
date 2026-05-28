#ifndef ATLAS_SERVER_CELLAPP_MOVEMENT_POSITION_HISTORY_STORE_H_
#define ATLAS_SERVER_CELLAPP_MOVEMENT_POSITION_HISTORY_STORE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

#include "movement_sim/movement_sim.h"
#include "server/entity_types.h"

namespace atlas {

struct MovementPositionSample {
  uint32_t server_tick{0};
  movement::MovementState state;
};

class MovementPositionHistoryStore {
 public:
  static constexpr std::size_t kDefaultMaxSamplesPerEntity = 30;

  explicit MovementPositionHistoryStore(
      std::size_t max_samples_per_entity = kDefaultMaxSamplesPerEntity);

  void Record(EntityID entity_id, uint32_t server_tick,
              const movement::MovementState& state);
  [[nodiscard]] auto Find(EntityID entity_id) const
      -> const std::deque<MovementPositionSample>*;
  [[nodiscard]] auto SampleAt(EntityID entity_id, uint32_t server_tick) const
      -> std::optional<MovementPositionSample>;
  void Erase(EntityID entity_id);

  [[nodiscard]] auto EntityCount() const -> std::size_t { return histories_.size(); }
  [[nodiscard]] auto TotalSampleCount() const -> std::size_t;
  [[nodiscard]] auto MaxSamplesPerEntity() const -> std::size_t {
    return max_samples_per_entity_;
  }

 private:
  std::unordered_map<EntityID, std::deque<MovementPositionSample>> histories_;
  std::size_t max_samples_per_entity_{kDefaultMaxSamplesPerEntity};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_MOVEMENT_POSITION_HISTORY_STORE_H_
