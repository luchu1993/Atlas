#ifndef ATLAS_TESTS_PARITY_PARITY_SCENARIO_H_
#define ATLAS_TESTS_PARITY_PARITY_SCENARIO_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "movement_sim/movement_sim.h"
#include "physics/physics_query.h"

namespace atlas::physics::parity {

enum class BackendKind : uint8_t { kFlat, kStatic, kJolt };

[[nodiscard]] inline auto Name(BackendKind kind) -> const char* {
  switch (kind) {
    case BackendKind::kFlat:
      return "Flat";
    case BackendKind::kStatic:
      return "Static";
    case BackendKind::kJolt:
      return "Jolt";
  }
  return "?";
}

struct ToleranceProfile {
  float position_eps_m{1e-3f};
  float velocity_eps_mps{1e-2f};
  float direction_dot_min{0.9999f};
  float cumulative_drift_m{5e-2f};
  // grounded flag is required to agree exactly except for `flag_lag_ticks`
  // worth of trailing single-tick mismatches around transitions.
  uint8_t flag_lag_ticks{0};
};

inline constexpr ToleranceProfile kStrictTolerance{
    .position_eps_m = 1e-4f,
    .velocity_eps_mps = 1e-3f,
    .direction_dot_min = 0.99999f,
    .cumulative_drift_m = 1e-2f,
    .flag_lag_ticks = 0,
};

inline constexpr ToleranceProfile kNormalTolerance{
    .position_eps_m = 1e-3f,
    .velocity_eps_mps = 1e-2f,
    .direction_dot_min = 0.9999f,
    .cumulative_drift_m = 5e-2f,
    .flag_lag_ticks = 0,
};

inline constexpr ToleranceProfile kMeshTolerance{
    .position_eps_m = 5e-3f,
    .velocity_eps_mps = 5e-2f,
    .direction_dot_min = 0.999f,
    .cumulative_drift_m = 2e-1f,
    .flag_lag_ticks = 1,
};

struct PerTickRecord {
  uint32_t tick{0};
  movement::MovementState state;
  movement::MovementStepResult step;
};

struct ParityResult {
  bool passed{true};
  std::optional<uint32_t> first_divergence_tick;
  std::string diff_summary;
};

// Each scenario contributes a single backend query factory; if the backend is
// not in `backends` or not compiled in, the runner skips it.
struct ParityScenario {
  std::string_view id;
  movement::MovementConfig config{};
  movement::MovementState initial_state{};
  // Inputs are cycled per tick (tick i uses inputs[i % inputs.size()]).
  std::vector<movement::InputFrame> inputs;
  uint32_t tick_count{0};
  float ground_probe_distance_m{2.0f};
  float ground_probe_radius_m{0.35f};
  // Layer mask the movement queries run with (default: all layers).
  LayerMask query_mask{};
  ToleranceProfile tolerance{kNormalTolerance};
  std::vector<BackendKind> backends;
  std::function<std::unique_ptr<PhysicsQuery>(BackendKind)> make_query;
};

}  // namespace atlas::physics::parity

#endif  // ATLAS_TESTS_PARITY_PARITY_SCENARIO_H_
