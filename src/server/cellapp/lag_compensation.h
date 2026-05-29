#ifndef ATLAS_SERVER_CELLAPP_LAG_COMPENSATION_H_
#define ATLAS_SERVER_CELLAPP_LAG_COMPENSATION_H_

#include <cstdint>
#include <optional>
#include <span>

#include "math/vector3.h"
#include "movement_position_history_store.h"
#include "server/entity_types.h"

namespace atlas {

// Favor-the-shooter prototype: rewind targets to the tick a shooter perceived
// them at, so a hit the shooter saw lands even though the target has since
// moved. See docs/gameplay/02_sync/LAG_COMPENSATION.md. PvP gating, iframe-state
// rewind, and boundary tolerance are the consumer's / later milestones' job.
struct LagCompensationConfig {
  uint32_t tick_hz{30};
  float interp_delay_ms{100.0f};                 // remote-entity display delay
  float input_to_hit_ms{1000.0f / 30.0f};        // ~1 tick from input to hitbox
  float max_rewind_ms{200.0f};                   // favor-the-shooter window cap
  float favor_shooter_tolerance_m{0.2f};         // boundary "almost hit" band (§3.3)
};

// The tick the shooter saw its target at: server_tick minus
// clamp(rtt/2 + interp_delay + input_to_hit, 0, max_rewind) converted to ticks.
// Clamped so the rewind never exceeds the window or underflows past tick 0.
[[nodiscard]] auto ComputeRewindTick(uint32_t server_tick, float rtt_ms,
                                     const LagCompensationConfig& cfg = {}) -> uint32_t;

struct LagCompCandidate {
  EntityID id{kInvalidEntityID};
  math::Vector3 current_position;  // fallback when no history sample exists
};

struct LagCompHit {
  EntityID id{kInvalidEntityID};
  math::Vector3 rewound_position;
  float distance_m{0.0f};
  bool from_history{false};  // false => fell back to current_position
  bool grazing{false};       // hit only within the favor-the-shooter band
};

// Rewinds each candidate to rewind_tick and returns the nearest whose rewound
// position is within (hit_radius_m + boundary_tolerance_m) of origin. A
// candidate with no history sample at rewind_tick uses its current position
// (cross-cell edge, §6.4). A hit beyond hit_radius_m but inside the tolerance
// band is reported with grazing=true (favor-the-shooter, §3.3).
[[nodiscard]] auto RewindSphereHit(const MovementPositionHistoryStore& history,
                                   std::span<const LagCompCandidate> candidates,
                                   uint32_t rewind_tick, const math::Vector3& origin,
                                   float hit_radius_m, float boundary_tolerance_m = 0.0f)
    -> std::optional<LagCompHit>;

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_LAG_COMPENSATION_H_
