// AoI hysteresis churn regression test.
//
// Verifies that a peer oscillating between the inner (aoi_radius) and
// outer (aoi_radius + hysteresis) bands does NOT flap enter/leave
// events. The dual-band trigger design is explicitly meant to let an
// entity sit on the boundary without generating per-tick envelopes.
//
// Scenario:
//   observer at origin, aoi_radius = 50m, hysteresis = 5m
//   peer starts at 60m (outside outer 55m) — no event expected
//   peer moves to 48m                        — 1 enter expected
//   peer bounces { 52, 48, 53, 49, 54, 48 } — 0 new events expected
//   peer moves to 60m (past outer)          — 1 leave expected
//
// If hysteresis is wired correctly, total envelopes observed through
// the entire oscillation phase is exactly 2 (1 enter, 1 leave). Any
// higher count means the band isn't suppressing repeated inner
// crossings and the AoI would churn on boundary-hugging peers.

#include <cstddef>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

struct CapturedEnvelope {
  EntityID observer_base_id;
  CellAoIEnvelopeKind kind;
};

auto KindFromBytes(std::span<const std::byte> env) -> CellAoIEnvelopeKind {
  return static_cast<CellAoIEnvelopeKind>(env[0]);
}

auto CountEnters(const std::vector<CapturedEnvelope>& env) -> int {
  int n = 0;
  for (const auto& e : env) {
    if (e.kind == CellAoIEnvelopeKind::kEntityEnter) ++n;
  }
  return n;
}

auto CountLeaves(const std::vector<CapturedEnvelope>& env) -> int {
  int n = 0;
  for (const auto& e : env) {
    if (e.kind == CellAoIEnvelopeKind::kEntityLeave) ++n;
  }
  return n;
}

auto MakeEntity(Space& space, EntityID id, math::Vector3 pos) -> CellEntity* {
  auto* e = space.AddEntity(std::make_unique<CellEntity>(id, /*type_id=*/uint16_t{1}, space, pos,
                                                         math::Vector3{1, 0, 0}));
  e->SetBase(Address(0, 0), /*base_id=*/id + 1000);
  return e;
}

// Drive Witness::Update so every peer-position change gets a chance
// to convert trigger events into captured envelopes before the next
// move. Each movement step ticks once.
TEST(AoIHysteresisChurn, PeerInBandDoesNotFlap) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  std::vector<CapturedEnvelope> sent;
  observer->EnableWitness(
      /*radius=*/50.f,
      [&sent](EntityID obs, std::span<const std::byte> env) {
        sent.push_back({obs, KindFromBytes(env)});
      },
      /*hysteresis=*/5.f);

  // Peer spawns deep inside the AoI, giving us a clean baseline of
  // exactly 1 enter before the churn loop. Spawning at the boundary
  // or past the outer band risks the insert-time shuffle firing
  // transient events that'd mask the hysteresis check.
  auto* peer = MakeEntity(space, 100, {10.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(CountEnters(sent), 1);
  ASSERT_EQ(CountLeaves(sent), 0);

  // Oscillate inside the hysteresis band. Positions all satisfy
  // inner_radius < |x| < outer_radius, i.e. 50 < x < 55. Each move
  // should produce no new enter (HandleAoIEnter dedups already-present
  // peers) and no leave (outer's OnLeave only fires past 55).
  const float kChurnMoves[] = {52.f, 48.f, 53.f, 49.f, 54.f, 48.f, 51.f, 49.5f};
  for (float x : kChurnMoves) {
    peer->SetPosition({x, 0.f, 0.f});
    observer->GetWitness()->Update(4096);
  }
  EXPECT_EQ(CountEnters(sent), 1) << "inner-band bounce must not re-fire enter";
  EXPECT_EQ(CountLeaves(sent), 0) << "outer-band bounce must not fire leave";

  // Cross the outer boundary outbound. One leave.
  peer->SetPosition({60.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountEnters(sent), 1);
  EXPECT_EQ(CountLeaves(sent), 1);

  // Come back in: a fresh enter fires (the previous leave cleared the
  // cache entry on Update; Witness treats the re-cross as a new peer).
  peer->SetPosition({45.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountEnters(sent), 2);
  EXPECT_EQ(CountLeaves(sent), 1);
}

// Tighter variant: peer parks exactly ON the inner boundary and
// wiggles by amounts smaller than hysteresis. This is the worst-case
// for a naive trigger that only looks at aoi_radius.
TEST(AoIHysteresisChurn, WiggleOnInnerBoundary_NoEvents) {
  Space space(2);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  std::vector<CapturedEnvelope> sent;
  observer->EnableWitness(
      100.f,
      [&sent](EntityID obs, std::span<const std::byte> env) {
        sent.push_back({obs, KindFromBytes(env)});
      },
      /*hysteresis=*/10.f);

  // Peer enters once from well inside so we have a baseline.
  auto* peer = MakeEntity(space, 200, {0.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  const auto baseline_enters = CountEnters(sent);
  const auto baseline_leaves = CountLeaves(sent);
  ASSERT_EQ(baseline_enters, 1);
  ASSERT_EQ(baseline_leaves, 0);

  // Wiggle the peer by <hysteresis/2 across 100m boundary, spending
  // time on both sides of the inner ring but never past outer (110m).
  const float kBoundary = 100.f;
  const float kWiggle[] = {-1.0f, +1.0f, -2.0f, +2.0f, -3.0f, +3.0f, -4.0f, +4.0f, -1.5f, +1.5f};
  for (float dx : kWiggle) {
    peer->SetPosition({kBoundary + dx, 0.f, 0.f});
    observer->GetWitness()->Update(4096);
  }
  EXPECT_EQ(CountEnters(sent), baseline_enters) << "wiggle across inner must not re-enter";
  EXPECT_EQ(CountLeaves(sent), baseline_leaves) << "wiggle inside outer must not leave";
}

}  // namespace
}  // namespace atlas
