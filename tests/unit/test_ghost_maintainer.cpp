// Phase 11 PR-4 — GhostMaintainer tests.
//
// Feeds the maintainer a hand-built Space + BSP tree + synthetic channel
// lookups, then inspects the returned PendingWork. No network involved;
// fake Channel* values are only compared for identity.

#include <cstdint>
#include <memory>
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

}  // namespace
}  // namespace atlas
