#include "movement_input_rate_limiter.h"

#include <algorithm>
#include <cmath>

namespace atlas {
namespace {

auto NonNegativeFinite(double value) -> double {
  return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

}  // namespace

MovementInputRateLimiter::MovementInputRateLimiter(double packets_per_second, double burst)
    : packets_per_second_(NonNegativeFinite(packets_per_second)),
      burst_(NonNegativeFinite(burst)) {}

auto MovementInputRateLimiter::Consume(EntityID entity_id, TimePoint now) -> bool {
  if (entity_id == kInvalidEntityID || burst_ < 1.0) return false;

  auto& bucket = buckets_[entity_id];
  if (!bucket.initialized) {
    bucket.refilled_at = now;
    bucket.tokens = burst_;
    bucket.initialized = true;
  }

  const double elapsed_s = std::max(0.0, Seconds(now - bucket.refilled_at).count());
  if (elapsed_s > 0.0) {
    bucket.tokens = std::min(burst_, bucket.tokens + elapsed_s * packets_per_second_);
    bucket.refilled_at = now;
  }
  if (bucket.tokens < 1.0) return false;

  bucket.tokens -= 1.0;
  return true;
}

void MovementInputRateLimiter::Erase(EntityID entity_id) {
  buckets_.erase(entity_id);
}

}  // namespace atlas
