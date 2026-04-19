// Phase 11 PR-4 — OffloadChecker tests.

#include <memory>

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
      std::make_unique<CellEntity>(id, /*type=*/1, space, pos, math::Vector3{1, 0, 0}));
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
  auto* g = space.AddEntity(
      std::make_unique<CellEntity>(CellEntity::GhostTag{}, 99, 1, space, math::Vector3{50, 0, 0},
                                   math::Vector3{1, 0, 0}, reinterpret_cast<Channel*>(0xBEEF)));
  space.FindLocalCell(1)->AddRealEntity(g);  // deliberately misroute

  OffloadChecker checker(self);
  EXPECT_TRUE(checker.Compute(space).empty());
}

}  // namespace
}  // namespace atlas
