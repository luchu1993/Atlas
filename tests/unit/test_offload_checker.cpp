// OffloadChecker tests.

#include <memory>
#include <set>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellappmgr/bsp_tree.h"
#include "math/vector3.h"
#include "offload_checker.h"
#include "space.h"

namespace atlas {
namespace {

auto MakeReal(Space& space, EntityID id, math::Vector3 pos) -> CellEntity* {
  return space.AddEntity(
      std::make_unique<CellEntity>(id, /*type=*/uint16_t{1}, space, pos, math::Vector3{1, 0, 0}));
}

auto BuildTopology(Space& space, const Address& self, const Address& peer,
                   cellappmgr::CellID self_cell_id) {
  BSPTree tree;
  CellInfo c1;
  c1.cell_id = self_cell_id;
  c1.cellapp_addr = self;
  tree.InitSingleCell(c1);

  CellInfo c2;
  c2.cell_id = 99;
  c2.cellapp_addr = peer;
  auto r = tree.Split(self_cell_id, BSPAxis::kX, 0.f, c2);
  EXPECT_TRUE(r.HasValue());

  space.SetBspTree(std::move(tree));
  auto local = std::make_unique<Cell>(space, self_cell_id, CellBounds{});
  space.AddLocalCell(std::move(local));
}

TEST(OffloadChecker, EntityInLocalCell_NoOp) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  BuildTopology(space, self, peer, /*self_cell_id=*/1);

  auto* e = MakeReal(space, 10, {-50.f, 0.f, 0.f});  // left side, our cell.
  space.FindLocalCell(1)->AddRealEntity(e);

  OffloadChecker checker(self);
  auto ops = checker.Compute(space);
  EXPECT_TRUE(ops.empty());
}

TEST(OffloadChecker, EntityCrossedIntoPeerCell_EmitsOffload) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  BuildTopology(space, self, peer, 1);

  auto* e = MakeReal(space, 10, {50.f, 0.f, 0.f});  // right side — peer's cell.
  space.FindLocalCell(1)->AddRealEntity(e);

  OffloadChecker checker(self);
  auto ops = checker.Compute(space);
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops[0].entity, e);
  EXPECT_EQ(ops[0].target_cellapp_addr, peer);
  EXPECT_EQ(ops[0].target_cell_id, 99u);
}

TEST(OffloadChecker, CellWithShouldOffloadFalse_Suppressed) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  BuildTopology(space, self, peer, 1);

  auto* e = MakeReal(space, 10, {50.f, 0.f, 0.f});
  auto* cell = space.FindLocalCell(1);
  cell->AddRealEntity(e);
  cell->SetShouldOffload(false);

  OffloadChecker checker(self);
  EXPECT_TRUE(checker.Compute(space).empty());
}

TEST(OffloadChecker, NoBspTree_NoOp) {
  Space space(1);
  auto local = std::make_unique<Cell>(space, 1, CellBounds{});
  auto* cell = space.AddLocalCell(std::move(local));
  auto* e = MakeReal(space, 10, {0, 0, 0});
  cell->AddRealEntity(e);

  OffloadChecker checker(Address{});
  EXPECT_TRUE(checker.Compute(space).empty());
}

TEST(OffloadChecker, GhostEntitiesNotEvenInLocalCell_NoOp) {
  // Ghosts aren't in Cell::real_entities_, so the checker never sees
  // them regardless of their position. This is belt-and-braces: verify
  // the membership filter is doing its job.
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  BuildTopology(space, self, peer, 1);

  // Ghost on the far side — if it somehow got into local_cell it'd fire
  // an Offload, so this test also exercises the defensive IsReal() gate
  // in the checker.
  auto* g = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 99, uint16_t{1}, space, math::Vector3{50, 0, 0},
      math::Vector3{1, 0, 0}, reinterpret_cast<Channel*>(0xBEEF)));
  space.FindLocalCell(1)->AddRealEntity(g);  // deliberately misroute

  OffloadChecker checker(self);
  EXPECT_TRUE(checker.Compute(space).empty());
}

// ============================================================================
// 3-cell topology: self (cell 1, x < 0), peer_a (cell 2, 0 <= x < 100),
// peer_b (cell 3, x >= 100). Self has one local Cell (cell 1).
// ============================================================================

auto BuildThreeCellTopology(Space& space, const Address& self, const Address& peer_a,
                            const Address& peer_b, cellappmgr::CellID self_cell_id) {
  BSPTree tree;
  CellInfo c1;
  c1.cell_id = self_cell_id;
  c1.cellapp_addr = self;
  tree.InitSingleCell(c1);

  // First split at x=0: self keeps left (x < 0), cell 2 gets right (x >= 0).
  CellInfo c2;
  c2.cell_id = 2;
  c2.cellapp_addr = peer_a;
  auto r1 = tree.Split(self_cell_id, BSPAxis::kX, 0.f, c2);
  EXPECT_TRUE(r1.HasValue());

  // Second split at x=100: cell 2 keeps left (0 <= x < 100), cell 3 gets right (x >= 100).
  CellInfo c3;
  c3.cell_id = 3;
  c3.cellapp_addr = peer_b;
  auto r2 = tree.Split(/*existing=*/2, BSPAxis::kX, 100.f, c3);
  EXPECT_TRUE(r2.HasValue());

  space.SetBspTree(std::move(tree));
  auto local = std::make_unique<Cell>(space, self_cell_id, CellBounds{});
  space.AddLocalCell(std::move(local));
}

// ============================================================================
// Two entities, each in a different peer's territory → 2 distinct offload ops.
// ============================================================================

TEST(OffloadChecker, MultiCellSpaceOffloadsToCorrectTarget) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer_a{0x7F000002u, 30002};
  Address peer_b{0x7F000003u, 30003};
  BuildThreeCellTopology(space, self, peer_a, peer_b, /*self_cell_id=*/1);

  // Entity A at x=50 → inside peer_a's territory (cell 2, 0 <= x < 100).
  auto* ea = MakeReal(space, 10, {50.f, 0.f, 0.f});
  space.FindLocalCell(1)->AddRealEntity(ea);

  // Entity B at x=200 → inside peer_b's territory (cell 3, x >= 100).
  auto* eb = MakeReal(space, 11, {200.f, 0.f, 0.f});
  space.FindLocalCell(1)->AddRealEntity(eb);

  OffloadChecker checker(self);
  auto ops = checker.Compute(space);
  ASSERT_EQ(ops.size(), 2u);

  // Collect target addresses and cell IDs; verify both peers are hit.
  std::set<Address> addrs;
  std::set<cellappmgr::CellID> cell_ids;
  for (const auto& op : ops) {
    addrs.insert(op.target_cellapp_addr);
    cell_ids.insert(op.target_cell_id);
  }
  EXPECT_TRUE(addrs.count(peer_a));
  EXPECT_TRUE(addrs.count(peer_b));
  EXPECT_TRUE(cell_ids.count(2u));
  EXPECT_TRUE(cell_ids.count(3u));
}

// ============================================================================
// Entity returning to own cell is not offloaded.
// ============================================================================

TEST(OffloadChecker, EntityReturningToOwnCellNotOffloaded) {
  Space space(1);
  Address self{0x7F000001u, 30001};
  Address peer{0x7F000002u, 30002};
  BuildTopology(space, self, peer, /*self_cell_id=*/1);

  // Step 1: entity starts in own cell (x < 0) → no offload.
  auto* e = MakeReal(space, 10, {-50.f, 0.f, 0.f});
  space.FindLocalCell(1)->AddRealEntity(e);

  OffloadChecker checker(self);
  EXPECT_TRUE(checker.Compute(space).empty());

  // Step 2: move entity into peer's territory → offload emitted.
  e->SetPosition({50.f, 0.f, 0.f});
  auto ops1 = checker.Compute(space);
  ASSERT_EQ(ops1.size(), 1u);
  EXPECT_EQ(ops1[0].entity, e);
  EXPECT_EQ(ops1[0].target_cellapp_addr, peer);

  // Step 3: move entity back to own cell → no offload.
  e->SetPosition({-50.f, 0.f, 0.f});
  EXPECT_TRUE(checker.Compute(space).empty());
}

}  // namespace
}  // namespace atlas
