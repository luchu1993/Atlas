#include "parity_runner.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace atlas::physics::parity {

namespace {

[[nodiscard]] auto Distance(const math::Vector3& a, const math::Vector3& b) -> float {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] auto Dot(const math::Vector3& a, const math::Vector3& b) -> float {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

}  // namespace

auto RunScenario(const ParityScenario& scenario, BackendKind backend)
    -> std::vector<PerTickRecord> {
  if (!scenario.make_query) return {};
  if (std::find(scenario.backends.begin(), scenario.backends.end(), backend) ==
      scenario.backends.end()) {
    return {};
  }
  auto query = scenario.make_query(backend);
  if (!query) return {};

  movement::PhysicsCharacterQuery character_query(*query, scenario.ground_probe_distance_m,
                                                  scenario.query_mask,
                                                  scenario.ground_probe_radius_m);
  movement::MovementState state = scenario.initial_state;
  std::vector<PerTickRecord> records;
  records.reserve(scenario.tick_count);

  const std::size_t input_count = std::max<std::size_t>(scenario.inputs.size(), 1);
  movement::InputFrame fallback;
  fallback.client_dt_ms = 33;

  for (uint32_t tick = 1; tick <= scenario.tick_count; ++tick) {
    movement::InputFrame input =
        scenario.inputs.empty() ? fallback : scenario.inputs[(tick - 1) % input_count];
    input.seq = tick;
    input.input_tick = tick;
    if (input.client_dt_ms == 0) input.client_dt_ms = 33;

    auto result = movement::Step(state, input, scenario.config, character_query, tick);
    state = result.state;
    records.push_back({tick, state, result});
  }
  return records;
}

auto ComparePair(const ParityScenario& scenario, const std::vector<PerTickRecord>& a,
                 const std::vector<PerTickRecord>& b) -> ParityResult {
  ParityResult out;
  if (a.empty() || b.empty()) {
    // Backend skipped (e.g. Jolt disabled); the gtest layer asserts viability.
    return out;
  }
  if (a.size() != b.size()) {
    out.passed = false;
    out.diff_summary =
        std::format("tick count mismatch: a={} b={}", a.size(), b.size());
    return out;
  }

  const auto& tol = scenario.tolerance;
  uint8_t grounded_lag = 0;

  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto& ra = a[i];
    const auto& rb = b[i];

    const float pos_diff = Distance(ra.state.position, rb.state.position);
    if (pos_diff > tol.position_eps_m) {
      out.passed = false;
      out.first_divergence_tick = ra.tick;
      out.diff_summary = std::format(
          "tick {} position diff {:.4f}m > {:.4f}m (a=({:.4f},{:.4f},{:.4f}) "
          "b=({:.4f},{:.4f},{:.4f}))",
          ra.tick, pos_diff, tol.position_eps_m, ra.state.position.x,
          ra.state.position.y, ra.state.position.z, rb.state.position.x,
          rb.state.position.y, rb.state.position.z);
      return out;
    }

    const float vel_diff = Distance(ra.state.velocity, rb.state.velocity);
    if (vel_diff > tol.velocity_eps_mps) {
      out.passed = false;
      out.first_divergence_tick = ra.tick;
      out.diff_summary = std::format("tick {} velocity diff {:.4f}m/s > {:.4f}m/s",
                                     ra.tick, vel_diff, tol.velocity_eps_mps);
      return out;
    }

    const float dot = Dot(ra.state.direction, rb.state.direction);
    if (dot < tol.direction_dot_min) {
      out.passed = false;
      out.first_divergence_tick = ra.tick;
      out.diff_summary = std::format("tick {} direction dot {:.5f} < {:.5f}", ra.tick,
                                     dot, tol.direction_dot_min);
      return out;
    }

    const bool a_grounded =
        (ra.state.flags & movement::kMovementFlagGrounded) != 0;
    const bool b_grounded =
        (rb.state.flags & movement::kMovementFlagGrounded) != 0;
    if (a_grounded != b_grounded) {
      ++grounded_lag;
      if (grounded_lag > tol.flag_lag_ticks) {
        out.passed = false;
        out.first_divergence_tick = ra.tick;
        out.diff_summary = std::format(
            "tick {} grounded mismatch sustained {} ticks (a={} b={})", ra.tick,
            grounded_lag, a_grounded ? 1 : 0, b_grounded ? 1 : 0);
        return out;
      }
    } else {
      grounded_lag = 0;
    }
  }

  const float drift = Distance(a.back().state.position, b.back().state.position);
  if (drift > tol.cumulative_drift_m) {
    out.passed = false;
    out.first_divergence_tick = a.back().tick;
    out.diff_summary =
        std::format("cumulative drift {:.4f}m > {:.4f}m at tick {}", drift,
                    tol.cumulative_drift_m, a.back().tick);
    return out;
  }
  return out;
}

}  // namespace atlas::physics::parity
