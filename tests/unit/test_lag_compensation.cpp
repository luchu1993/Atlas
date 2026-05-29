#include <vector>

#include <gtest/gtest.h>

#include "lag_compensation.h"
#include "math/vector3.h"
#include "movement_position_history_store.h"

namespace atlas {
namespace {

auto SampleAt(uint32_t tick, math::Vector3 pos) -> movement::MovementState {
  movement::MovementState s;
  s.position = pos;
  return s;
}

TEST(LagCompensation, RewindTickSubtractsPerceivedDelay) {
  LagCompensationConfig cfg;  // 30 Hz, interp 100ms, input ~33ms, cap 200ms
  // RTT 0 → 0 + 100 + 33 = 133ms ≈ 4 ticks at 33.3ms/tick.
  EXPECT_EQ(ComputeRewindTick(1000, 0.0f, cfg), 1000u - 4u);
  // RTT 120 → 60 + 100 + 33 = 193ms ≈ 6 ticks.
  EXPECT_EQ(ComputeRewindTick(1000, 120.0f, cfg), 1000u - 6u);
}

TEST(LagCompensation, RewindClampsToMaxWindow) {
  LagCompensationConfig cfg;
  // RTT 400 → 200 + 100 + 33 = 333ms, clamped to 200ms = 6 ticks.
  EXPECT_EQ(ComputeRewindTick(1000, 400.0f, cfg), 1000u - 6u);
  // Even huge RTT never rewinds more than the 200ms window.
  EXPECT_EQ(ComputeRewindTick(1000, 100000.0f, cfg), 1000u - 6u);
}

TEST(LagCompensation, RewindNeverUnderflowsPastTickZero) {
  LagCompensationConfig cfg;
  EXPECT_EQ(ComputeRewindTick(2, 400.0f, cfg), 0u);
}

TEST(LagCompensation, HitsTargetAtItsPastPositionAfterItMovedAway) {
  MovementPositionHistoryStore history;
  // Entity walked from origin (tick 10) to x=5 (tick 16).
  history.Record(99, 10, SampleAt(10, {0.0f, 0.0f, 0.0f}));
  history.Record(99, 16, SampleAt(16, {5.0f, 0.0f, 0.0f}));

  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{99, math::Vector3{5.0f, 0.0f, 0.0f}}};  // current pos = where it is now
  const math::Vector3 origin{0.2f, 0.0f, 0.0f};               // shooter aimed at the old spot

  // No rewind (current position x=5) → 4.8m away → miss.
  auto now = RewindSphereHit(history, candidates, 16, origin, 1.0f);
  EXPECT_FALSE(now.has_value());

  // Rewind to tick 10 (target was at origin) → hit.
  auto past = RewindSphereHit(history, candidates, 10, origin, 1.0f);
  ASSERT_TRUE(past.has_value());
  EXPECT_EQ(past->id, 99u);
  EXPECT_TRUE(past->from_history);
  EXPECT_NEAR(past->rewound_position.x, 0.0f, 1e-4f);
}

TEST(LagCompensation, InterpolatesBetweenSamples) {
  MovementPositionHistoryStore history;
  history.Record(7, 10, SampleAt(10, {0.0f, 0.0f, 0.0f}));
  history.Record(7, 20, SampleAt(20, {10.0f, 0.0f, 0.0f}));

  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{7, math::Vector3{10.0f, 0.0f, 0.0f}}};
  // Tick 15 interpolates halfway → x=5.
  auto hit = RewindSphereHit(history, candidates, 15, {5.0f, 0.0f, 0.0f}, 0.5f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_NEAR(hit->rewound_position.x, 5.0f, 1e-3f);
}

TEST(LagCompensation, FallsBackToCurrentPositionWithoutHistory) {
  MovementPositionHistoryStore history;  // empty
  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{42, math::Vector3{0.0f, 0.0f, 0.0f}}};
  auto hit = RewindSphereHit(history, candidates, 100, {0.0f, 0.0f, 0.0f}, 1.0f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_FALSE(hit->from_history);  // used current_position
}

TEST(LagCompensation, ReturnsNearestCandidate) {
  MovementPositionHistoryStore history;
  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{1, math::Vector3{0.9f, 0.0f, 0.0f}},
      LagCompCandidate{2, math::Vector3{0.3f, 0.0f, 0.0f}}};
  auto hit = RewindSphereHit(history, candidates, 0, {0.0f, 0.0f, 0.0f}, 1.0f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->id, 2u);  // 0.3m beats 0.9m
}

TEST(LagCompensation, FavorShooterToleranceCountsGrazingHit) {
  MovementPositionHistoryStore history;
  // Target 1.1m away: outside a 1.0m radius, inside the 0.2m favor band.
  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{5, math::Vector3{1.1f, 0.0f, 0.0f}}};
  const math::Vector3 origin{0.0f, 0.0f, 0.0f};

  EXPECT_FALSE(RewindSphereHit(history, candidates, 0, origin, 1.0f).has_value());

  auto hit = RewindSphereHit(history, candidates, 0, origin, 1.0f, 0.2f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_TRUE(hit->grazing);  // beyond the core radius, within the band
}

TEST(LagCompensation, FavorShooterToleranceRejectsBeyondBand) {
  MovementPositionHistoryStore history;
  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{5, math::Vector3{1.3f, 0.0f, 0.0f}}};  // 0.3m past radius > 0.2 band
  EXPECT_FALSE(
      RewindSphereHit(history, candidates, 0, {0.0f, 0.0f, 0.0f}, 1.0f, 0.2f).has_value());
}

TEST(LagCompensation, CoreHitIsNotMarkedGrazing) {
  MovementPositionHistoryStore history;
  std::vector<LagCompCandidate> candidates = {
      LagCompCandidate{5, math::Vector3{0.5f, 0.0f, 0.0f}}};  // well inside the radius
  auto hit = RewindSphereHit(history, candidates, 0, {0.0f, 0.0f, 0.0f}, 1.0f, 0.2f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_FALSE(hit->grazing);
}

}  // namespace
}  // namespace atlas
