// Phase 11 PR-4 — GhostMaintainer tests.
//
// Feeds the maintainer a hand-built Space + BSP tree + synthetic channel
// lookups, then inspects the returned PendingWork. No network involved;
// fake Channel* values are only compared for identity.

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellappmgr/bsp_tree.h"
#include "ghost_maintainer.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "real_entity_data.h"
#include "space.h"

namespace atlas {
namespace {

auto FakeChannel(uintptr_t tag) -> Channel* {
  return reinterpret_cast<Channel*>(tag);
}

// A tiny two-peer topology:
//   self CellApp owns cell 1 covering x < 0
//   peer A owns cell 2 covering x >= 0
// Any Real near the x=0 boundary will want a Ghost on peer A.
auto BuildTwoCellTopology(Space& space, const Address& self_addr, const Address& peer_a_addr)
    -> BSPTree {
  BSPTree tree;
  CellInfo c1;
  c1.cell_id = 1;
  c1.cellapp_addr = self_addr;
  tree.InitSingleCell(c1);
  CellInfo c2;
  c2.cell_id = 2;
  c2.cellapp_addr = peer_a_addr;
  auto r = tree.Split(/*existing=*/1, BSPAxis::kX, /*position=*/0.f, c2);
  EXPECT_TRUE(r.HasValue());
  (void)space;  // BSP lives alongside Space but the space itself holds no tree here.
  return tree;
}

auto MakeReal(Space& space, EntityID id, math::Vector3 pos) -> CellEntity* {
  return space.AddEntity(
      std::make_unique<CellEntity>(id, /*type=*/1, space, pos, math::Vector3{1, 0, 0}));
}

// ============================================================================
// Baseline: entity deep inside own cell → no creates, no deletes.
// ============================================================================

TEST(GhostMaintainer, RealDeepInsideOwnCell_NoWork) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  auto* e = MakeReal(space, 10, {-1000.f, 0.f, 0.f});  // far from boundary
  (void)e;

  std::unordered_map<Address, Channel*> peers;
  peers[peer] = FakeChannel(0xA);
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::milliseconds(0)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  auto work = mgr.Run(space, Clock::now());
  EXPECT_TRUE(work.creates.empty());
  EXPECT_TRUE(work.deletes.empty());
}

// ============================================================================
// Entity near boundary → one CreateOp for the peer.
// ============================================================================

TEST(GhostMaintainer, NearBoundary_EmitsCreate) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  auto* e = MakeReal(space, 10, {-50.f, 0.f, 0.f});  // within 100m of x=0 boundary

  std::unordered_map<Address, Channel*> peers;
  peers[peer] = FakeChannel(0xA);
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::milliseconds(0)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  auto work = mgr.Run(space, Clock::now());
  ASSERT_EQ(work.creates.size(), 1u);
  EXPECT_EQ(work.creates[0].entity, e);
  EXPECT_EQ(work.creates[0].peer_addr, peer);
  EXPECT_EQ(work.creates[0].peer_cell_id, 2u);
  EXPECT_TRUE(work.deletes.empty());
}

// ============================================================================
// Existing Haunt on peer → no duplicate create.
// ============================================================================

TEST(GhostMaintainer, ExistingHaunt_NoDuplicateCreate) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  auto* e = MakeReal(space, 10, {-50.f, 0.f, 0.f});
  auto* peer_channel = FakeChannel(0xA);
  e->GetRealData()->AddHaunt(peer_channel);

  std::unordered_map<Address, Channel*> peers;
  peers[peer] = peer_channel;
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::milliseconds(0)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  auto work = mgr.Run(space, Clock::now());
  EXPECT_TRUE(work.creates.empty());
  EXPECT_TRUE(work.deletes.empty());
}

// ============================================================================
// Entity moves deep → haunt past min-lifespan gets deleted.
// ============================================================================

TEST(GhostMaintainer, HauntPastMinLifespan_OutsideInterest_EmitsDelete) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  auto* e = MakeReal(space, 10, {-5000.f, 0.f, 0.f});  // very far from boundary
  auto* peer_channel = FakeChannel(0xA);
  e->GetRealData()->AddHaunt(peer_channel);

  // Advance `now` past min-lifespan to allow deletion.
  std::unordered_map<Address, Channel*> peers;
  peers[peer] = peer_channel;
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::milliseconds(10)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  const auto later = Clock::now() + std::chrono::milliseconds(100);
  auto work = mgr.Run(space, later);
  EXPECT_TRUE(work.creates.empty());
  ASSERT_EQ(work.deletes.size(), 1u);
  EXPECT_EQ(work.deletes[0].entity, e);
  EXPECT_EQ(work.deletes[0].peer_channel, peer_channel);
}

// ============================================================================
// Entity deep but haunt too young → no delete (min-lifespan guard).
// ============================================================================

TEST(GhostMaintainer, HauntWithinMinLifespan_NoDelete) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  auto* e = MakeReal(space, 10, {-5000.f, 0.f, 0.f});
  auto* peer_channel = FakeChannel(0xA);
  e->GetRealData()->AddHaunt(peer_channel);

  std::unordered_map<Address, Channel*> peers;
  peers[peer] = peer_channel;
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::seconds(10)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  auto work = mgr.Run(space, Clock::now());
  EXPECT_TRUE(work.deletes.empty());
}

// ============================================================================
// Ghost entities (non-Real) are skipped entirely.
// ============================================================================

TEST(GhostMaintainer, GhostEntitiesAreSkipped) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  // Put a Ghost right next to the boundary; maintainer must not try to
  // create ghosts-of-a-ghost on the peer side.
  space.AddEntity(std::make_unique<CellEntity>(CellEntity::GhostTag{}, 99, 1, space,
                                               math::Vector3{-50, 0, 0}, math::Vector3{1, 0, 0},
                                               FakeChannel(0xAA)));
  std::unordered_map<Address, Channel*> peers;
  peers[peer] = FakeChannel(0xA);
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::milliseconds(0)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  auto work = mgr.Run(space, Clock::now());
  EXPECT_TRUE(work.creates.empty());
  EXPECT_TRUE(work.deletes.empty());
}

// ============================================================================
// No BSP tree → no-op (fresh space before UpdateGeometry arrives).
// ============================================================================

TEST(GhostMaintainer, NoBspTree_NoWork) {
  Space space(1);
  MakeReal(space, 10, {0, 0, 0});
  GhostMaintainer::Config cfg{};
  GhostMaintainer mgr(cfg, Address{}, [](const Address&) -> Channel* { return nullptr; });
  auto work = mgr.Run(space, Clock::now());
  EXPECT_TRUE(work.creates.empty());
  EXPECT_TRUE(work.deletes.empty());
}

// ============================================================================
// 3-cell topology helper: self at center, peer_a left, peer_b right.
//
//   First split on X at 50: left = self (cell 1), right = temp (cell 2).
//   Second split on X at 100: left = peer_a (cell 2), right = peer_b (cell 3).
//   Self covers [−inf, 50), peer_a covers [50, 100), peer_b covers [100, +inf).
// ============================================================================

auto BuildThreeCellTopology(Space& space, const Address& self_addr, const Address& peer_a_addr,
                            const Address& peer_b_addr) -> BSPTree {
  BSPTree tree;
  CellInfo c1;
  c1.cell_id = 1;
  c1.cellapp_addr = self_addr;
  tree.InitSingleCell(c1);

  // First split: cell 1 keeps left (x < 50), new cell 2 gets right (x >= 50).
  CellInfo c2;
  c2.cell_id = 2;
  c2.cellapp_addr = peer_a_addr;
  auto r1 = tree.Split(/*existing=*/1, BSPAxis::kX, /*position=*/50.f, c2);
  EXPECT_TRUE(r1.HasValue());

  // Second split: cell 2 keeps left (50 <= x < 100), new cell 3 gets right (x >= 100).
  CellInfo c3;
  c3.cell_id = 3;
  c3.cellapp_addr = peer_b_addr;
  auto r2 = tree.Split(/*existing=*/2, BSPAxis::kX, /*position=*/100.f, c3);
  EXPECT_TRUE(r2.HasValue());

  (void)space;
  return tree;
}

// ============================================================================
// Entity near both boundaries → creates emitted for BOTH peers.
// ============================================================================

TEST(GhostMaintainer, MultiPeerHauntCreation) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer_a{0x7F000002u, 30002};
  Address peer_b{0x7F000003u, 30003};
  space.SetBspTree(BuildThreeCellTopology(space, self, peer_a, peer_b));

  // Place entity at x=25 — within ghost_distance (100) of both the x=50
  // boundary (peer_a) and the x=100 boundary (peer_b).
  auto* e = MakeReal(space, 10, {25.f, 0.f, 0.f});

  std::unordered_map<Address, Channel*> peers;
  peers[peer_a] = FakeChannel(0xA);
  peers[peer_b] = FakeChannel(0xB);
  GhostMaintainer::Config cfg{100.f, 20.f, std::chrono::milliseconds(0)};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  auto work = mgr.Run(space, Clock::now());
  ASSERT_EQ(work.creates.size(), 2u);
  EXPECT_TRUE(work.deletes.empty());

  // Both creates must reference the same entity but distinct peers.
  EXPECT_EQ(work.creates[0].entity, e);
  EXPECT_EQ(work.creates[1].entity, e);

  // Collect the two target addresses; verify both peers are present.
  std::set<Address> targets;
  targets.insert(work.creates[0].peer_addr);
  targets.insert(work.creates[1].peer_addr);
  EXPECT_TRUE(targets.count(peer_a));
  EXPECT_TRUE(targets.count(peer_b));
}

// ============================================================================
// Position oscillation respects min-lifespan guard: create → suppress delete
// while young → allow delete when old → re-create on return.
// ============================================================================

TEST(GhostMaintainer, PositionOscillationRespectsMinLifespan) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  space.SetBspTree(BuildTwoCellTopology(space, self, peer));

  // Step 1: entity near boundary → create haunt.
  auto* e = MakeReal(space, 10, {-50.f, 0.f, 0.f});  // within 100m of x=0

  auto* peer_channel = FakeChannel(0xA);
  std::unordered_map<Address, Channel*> peers;
  peers[peer] = peer_channel;

  const auto min_lifespan = std::chrono::milliseconds(500);
  GhostMaintainer::Config cfg{100.f, 20.f, min_lifespan};
  GhostMaintainer mgr(cfg, self, [&](const Address& a) -> Channel* {
    auto it = peers.find(a);
    return it == peers.end() ? nullptr : it->second;
  });

  const auto t0 = Clock::now();
  auto work1 = mgr.Run(space, t0);
  ASSERT_EQ(work1.creates.size(), 1u);
  EXPECT_TRUE(work1.deletes.empty());

  // Apply the haunt that was created.
  e->GetRealData()->AddHaunt(peer_channel);

  // Step 2: move entity far from boundary.
  e->SetPosition({-5000.f, 0.f, 0.f});

  // Run with timestamp BEFORE min_lifespan expires → no delete.
  const auto t1 = t0 + std::chrono::milliseconds(100);
  auto work2 = mgr.Run(space, t1);
  EXPECT_TRUE(work2.creates.empty());
  EXPECT_TRUE(work2.deletes.empty());

  // Step 3: run with timestamp PAST min_lifespan → delete emitted.
  const auto t2 = t0 + std::chrono::milliseconds(600);
  auto work3 = mgr.Run(space, t2);
  EXPECT_TRUE(work3.creates.empty());
  ASSERT_EQ(work3.deletes.size(), 1u);
  EXPECT_EQ(work3.deletes[0].entity, e);
  EXPECT_EQ(work3.deletes[0].peer_channel, peer_channel);

  // Apply the delete.
  e->GetRealData()->RemoveHaunt(peer_channel);

  // Step 4: move entity BACK near boundary → create emitted again.
  e->SetPosition({-50.f, 0.f, 0.f});
  auto work4 = mgr.Run(space, t2 + std::chrono::milliseconds(10));
  ASSERT_EQ(work4.creates.size(), 1u);
  EXPECT_EQ(work4.creates[0].entity, e);
  EXPECT_EQ(work4.creates[0].peer_addr, peer);
  EXPECT_TRUE(work4.deletes.empty());
}

}  // namespace
}  // namespace atlas
