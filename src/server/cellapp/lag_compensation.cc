#include "lag_compensation.h"

#include <algorithm>
#include <cmath>

namespace atlas {

auto ComputeRewindTick(uint32_t server_tick, float rtt_ms,
                       const LagCompensationConfig& cfg) -> uint32_t {
  const float rtt = std::isfinite(rtt_ms) && rtt_ms > 0.0f ? rtt_ms : 0.0f;
  const float rewind_ms =
      std::clamp(rtt * 0.5f + cfg.interp_delay_ms + cfg.input_to_hit_ms, 0.0f,
                 cfg.max_rewind_ms);
  const float ms_per_tick = cfg.tick_hz > 0 ? 1000.0f / static_cast<float>(cfg.tick_hz) : 0.0f;
  if (ms_per_tick <= 0.0f) return server_tick;
  const auto rewind_ticks = static_cast<uint32_t>(std::lround(rewind_ms / ms_per_tick));
  return rewind_ticks >= server_tick ? 0u : server_tick - rewind_ticks;
}

auto RewindSphereHit(const MovementPositionHistoryStore& history,
                     std::span<const LagCompCandidate> candidates, uint32_t rewind_tick,
                     const math::Vector3& origin, float hit_radius_m)
    -> std::optional<LagCompHit> {
  if (!(hit_radius_m > 0.0f)) return std::nullopt;
  const float radius_sq = hit_radius_m * hit_radius_m;

  std::optional<LagCompHit> best;
  for (const auto& candidate : candidates) {
    if (candidate.id == kInvalidEntityID) continue;

    math::Vector3 position = candidate.current_position;
    bool from_history = false;
    if (auto sample = history.SampleAt(candidate.id, rewind_tick)) {
      position = sample->state.position;
      from_history = true;
    }

    const float dist_sq = (position - origin).LengthSquared();
    if (dist_sq > radius_sq) continue;
    if (!best || dist_sq < best->distance_m * best->distance_m) {
      best = LagCompHit{candidate.id, position, std::sqrt(dist_sq), from_history};
    }
  }
  return best;
}

}  // namespace atlas
