// AoI visibility precision test.
//
// Verifies that Witness::aoi_map_ contains EXACTLY the set of peers
// expected to be visible, not "most peers" or "some approximation".
// Counters in world_stress only track enter/leave deltas; this test
// pins the per-peer membership so a regression that leaks or misses
// a peer shows up immediately.
//
// The AoI trigger uses an axis-aligned square of side 2·radius, so
// "inside" means both |Δx| < radius AND |Δz| < radius. The test
// exercises corner cases: peers exactly at the diagonal, at the
// boundary of one axis, and past it.

#include <memory>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

auto MakeEntity(Space& space, EntityID id, math::Vector3 pos) -> CellEntity* {
  auto* e = space.AddEntity(
      std::make_unique<CellEntity>(id, /*type_id=*/1, space, pos, math::Vector3{1, 0, 0}));
  e->SetBase(Address(0, 0), /*base_id=*/id);
  return e;
}

// Collect every peer the witness currently tracks, ignoring caches
// marked kGone (those are pending-leave — not visible any more).
auto VisiblePeers(const Witness& w) -> std::set<EntityID> {
  std::set<EntityID> out;
  for (const auto& [id, cache] : w.AoIMap()) {
    if ((cache.flags & Witness::EntityCache::kGone) != 0) continue;
    out.insert(id);
  }
  return out;
}

// ============================================================================
// Static population: N peers at known positions → assert exact set.
// ============================================================================

TEST(AoIVisibility, InsidePeersMatchExpectedSet) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/10.f, [](EntityID, std::span<const std::byte>) {},
                          /*hysteresis=*/0.f);

  // Three peers inside the 10 m square, three outside, one on the edge
  // (edge is a strict < bound so it's OUTSIDE by design — asserts the
  // boundary semantics).
  auto* inside_a = MakeEntity(space, 100, {5.f, 0.f, 5.f});
  auto* inside_b = MakeEntity(space, 101, {-7.f, 0.f, 3.f});
  auto* inside_c = MakeEntity(space, 102, {9.5f, 0.f, 9.5f});
  auto* outside_x = MakeEntity(space, 200, {11.f, 0.f, 0.f});
  auto* outside_z = MakeEntity(space, 201, {0.f, 0.f, 15.f});
  auto* outside_diag = MakeEntity(space, 202, {20.f, 0.f, 20.f});
  auto* boundary = MakeEntity(space, 203, {10.f, 0.f, 0.f});  // exactly on edge → outside
  (void)outside_x;
  (void)outside_z;
  (void)outside_diag;
  (void)boundary;

  const std::set<EntityID> expected{inside_a->Id(), inside_b->Id(), inside_c->Id()};
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()), expected);
}

// ============================================================================
// Dynamic moves: walking peers in and out flips the set predictably.
// ============================================================================

TEST(AoIVisibility, MovesUpdateMembership) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {}, /*hysteresis=*/0.f);

  auto* p = MakeEntity(space, 100, {5.f, 0.f, 5.f});  // inside
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()), (std::set<EntityID>{p->Id()}));

  // Walk it out across the X edge.
  p->SetPosition({15.f, 0.f, 5.f});
  // HandleAoILeave marks the cache kGone; Update is needed to emit the
  // actual Leave envelope AND to erase the cache. VisiblePeers filters
  // kGone explicitly so we see the semantic "no longer visible" state
  // immediately after the move.
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()), std::set<EntityID>{});

  // Walk it back in.
  p->SetPosition({2.f, 0.f, 2.f});
  observer->GetWitness()->Update(4096);  // flush any pending Leave first
  p->SetPosition({3.f, 0.f, 3.f});       // nudge to re-emit the enter after pending cleanup
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()), (std::set<EntityID>{p->Id()}));
}

// ============================================================================
// SetAoIRadius shrink: peers now outside the new (smaller) square
// immediately drop out of the visible set, matching the new contract.
// Radius expansion brings them back.
// ============================================================================

TEST(AoIVisibility, SetAoIRadiusAdjustsSet) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {}, /*hysteresis=*/0.f);

  auto* near_peer = MakeEntity(space, 100, {3.f, 0.f, 3.f});
  auto* far_peer = MakeEntity(space, 101, {8.f, 0.f, 8.f});

  // Both inside 10 m square.
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()),
            (std::set<EntityID>{near_peer->Id(), far_peer->Id()}));

  // Shrink radius to 5 m: far_peer (8,8) now outside, near_peer still in.
  observer->GetWitness()->SetAoIRadius(5.f, 0.f);
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()), (std::set<EntityID>{near_peer->Id()}));

  // Expand back: far_peer rejoins.
  observer->GetWitness()->SetAoIRadius(10.f, 0.f);
  EXPECT_EQ(VisiblePeers(*observer->GetWitness()),
            (std::set<EntityID>{near_peer->Id(), far_peer->Id()}));
}

// ============================================================================
// Multiple observers: each keeps its own aoi_map independently.
// ============================================================================

TEST(AoIVisibility, ObserversTrackIndependentSets) {
  Space space(1);
  auto* observer_a = MakeEntity(space, 1, {0.f, 0.f, 0.f});
  auto* observer_b = MakeEntity(space, 2, {100.f, 0.f, 0.f});
  observer_a->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {}, 0.f);
  observer_b->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {}, 0.f);

  auto* near_a = MakeEntity(space, 100, {3.f, 0.f, 3.f});    // near A only
  auto* near_b = MakeEntity(space, 101, {103.f, 0.f, 0.f});  // near B only
  auto* between = MakeEntity(space, 102, {50.f, 0.f, 0.f});  // near neither
  (void)between;

  const std::set<EntityID> expected_a{near_a->Id()};
  const std::set<EntityID> expected_b{near_b->Id()};
  EXPECT_EQ(VisiblePeers(*observer_a->GetWitness()), expected_a);
  EXPECT_EQ(VisiblePeers(*observer_b->GetWitness()), expected_b);
}

}  // namespace
}  // namespace atlas
