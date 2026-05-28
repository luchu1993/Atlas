#ifndef ATLAS_SERVER_CELLAPP_MOVEMENT_INPUT_RATE_LIMITER_H_
#define ATLAS_SERVER_CELLAPP_MOVEMENT_INPUT_RATE_LIMITER_H_

#include <unordered_map>

#include "foundation/clock.h"
#include "server/entity_types.h"

namespace atlas {

class MovementInputRateLimiter {
 public:
  MovementInputRateLimiter(double packets_per_second = 45.0, double burst = 15.0);

  [[nodiscard]] auto Consume(EntityID entity_id, TimePoint now) -> bool;
  void Erase(EntityID entity_id);

 private:
  struct Bucket {
    TimePoint refilled_at{};
    double tokens{0.0};
    bool initialized{false};
  };

  double packets_per_second_{0.0};
  double burst_{0.0};
  std::unordered_map<EntityID, Bucket> buckets_;
};

}  // namespace atlas

#endif
