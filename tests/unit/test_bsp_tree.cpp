// BSP tree — Phase 11 PR-2.
//
// Covers: FindCell, VisitRect, Split, Balance (including aggression damping
// and safety-bound guard), Serialize/Deserialize round-trip. Loads are
// assumed to come from InformCellLoad — tests poke them in directly.

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cellappmgr/bsp_tree.h"
#include "serialization/binary_stream.h"

using namespace atlas;

namespace {

auto MakeLeafInfo(cellappmgr::CellID id, uint16_t port) -> CellInfo {
  CellInfo info;
  info.cell_id = id;
  info.cellapp_addr = Address(0x7F000001u, port);
  return info;
}

auto MakeSingleCellTree(cellappmgr::CellID id, uint16_t port) -> BSPTree {
  BSPTree t;
  t.InitSingleCell(MakeLeafInfo(id, port));
  return t;
}

}  // namespace

// ─── Single leaf ──────────────────────────────────────────────────────────────

TEST(BSPTree, InitSingleCell_FindsEverywhere) {
  auto t = MakeSingleCellTree(1, 30001);
  const auto* a = t.FindCell(0.f, 0.f);
  const auto* b = t.FindCell(-1e6f, 1e6f);
  const auto* c = t.FindCell(999.f, -999.f);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->cell_id, 1u);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->cell_id, 1u);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->cell_id, 1u);
}

// ─── Split on X axis ─────────────────────────────────────────────────────────

TEST(BSPTree, SplitX_RoutesByCoordinate) {
  auto t = MakeSingleCellTree(1, 30001);
  auto r = t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002));
  ASSERT_TRUE(r.HasValue());

  // Strictly negative x → left leaf (cell 1). Non-negative → right (2).
  ASSERT_NE(t.FindCell(-1.f, 0.f), nullptr);
  EXPECT_EQ(t.FindCell(-1.f, 0.f)->cell_id, 1u);
  ASSERT_NE(t.FindCell(1.f, 0.f), nullptr);
  EXPECT_EQ(t.FindCell(1.f, 0.f)->cell_id, 2u);
  // Boundary is half-open on the right — x == 0 goes to right (2).
  ASSERT_NE(t.FindCell(0.f, 0.f), nullptr);
  EXPECT_EQ(t.FindCell(0.f, 0.f)->cell_id, 2u);
}

TEST(BSPTree, SplitX_LeafBoundsReflectSplit) {
  auto t = MakeSingleCellTree(1, 30001);
  auto r = t.Split(1, BSPAxis::kX, 50.f, MakeLeafInfo(2, 30002));
  ASSERT_TRUE(r.HasValue());

  const auto* left = t.FindCellById(1);
  const auto* right = t.FindCellById(2);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_FLOAT_EQ(left->bounds.max_x, 50.f);
  EXPECT_FLOAT_EQ(right->bounds.min_x, 50.f);
  // z bounds stay ±inf.
  EXPECT_TRUE(std::isinf(left->bounds.min_z));
  EXPECT_TRUE(std::isinf(right->bounds.max_z));
}

TEST(BSPTree, Split_RejectsUnknownCellId) {
  auto t = MakeSingleCellTree(1, 30001);
  auto r = t.Split(42, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002));
  ASSERT_FALSE(r.HasValue());
  EXPECT_EQ(r.Error().Code(), ErrorCode::kNotFound);
}

TEST(BSPTree, Split_RejectsPositionOutsideBounds) {
  auto t = MakeSingleCellTree(1, 30001);
  // First split gives cell 1 bounds max_x = 50 (it takes the left side).
  auto r1 = t.Split(1, BSPAxis::kX, 50.f, MakeLeafInfo(2, 30002));
  ASSERT_TRUE(r1.HasValue());
  // Trying to split cell 1 at x=75 — but cell 1 now only covers x < 50.
  auto r2 = t.Split(1, BSPAxis::kX, 75.f, MakeLeafInfo(3, 30003));
  ASSERT_FALSE(r2.HasValue());
  EXPECT_EQ(r2.Error().Code(), ErrorCode::kInvalidArgument);
}

// ─── Multi-level split ───────────────────────────────────────────────────────

TEST(BSPTree, MultiLevelSplit_FindRoutesThroughNestedInternals) {
  // Layout target:
  //   root split X at 0 → (cell 1 | right)
  //   right  split Z at 0 → (cell 2 | cell 3)
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());
  ASSERT_TRUE(t.Split(2, BSPAxis::kZ, 0.f, MakeLeafInfo(3, 30003)).HasValue());

  // x < 0 → cell 1 regardless of z.
  EXPECT_EQ(t.FindCell(-10.f, -10.f)->cell_id, 1u);
  EXPECT_EQ(t.FindCell(-10.f, 10.f)->cell_id, 1u);
  // x >= 0, z < 0 → cell 2.
  EXPECT_EQ(t.FindCell(10.f, -5.f)->cell_id, 2u);
  // x >= 0, z >= 0 → cell 3.
  EXPECT_EQ(t.FindCell(10.f, 5.f)->cell_id, 3u);
}

// ─── VisitRect ───────────────────────────────────────────────────────────────

TEST(BSPTree, VisitRect_RectInsideOneCellVisitsOnlyThat) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());

  std::vector<cellappmgr::CellID> visited;
  t.VisitRect({-50.f, -50.f, -10.f, 10.f},
              [&](const CellInfo& ci) { visited.push_back(ci.cell_id); });
  ASSERT_EQ(visited.size(), 1u);
  EXPECT_EQ(visited[0], 1u);
}

TEST(BSPTree, VisitRect_RectStraddlingSplitVisitsBoth) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());

  std::vector<cellappmgr::CellID> visited;
  t.VisitRect({-10.f, -10.f, 10.f, 10.f},
              [&](const CellInfo& ci) { visited.push_back(ci.cell_id); });
  std::sort(visited.begin(), visited.end());
  ASSERT_EQ(visited.size(), 2u);
  EXPECT_EQ(visited[0], 1u);
  EXPECT_EQ(visited[1], 2u);
}

// ─── Balance — load-driven split line movement ───────────────────────────────

TEST(BSPTree, Balance_LeftHeavierShiftsRight) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());

  auto* left = t.FindCellByIdMutable(1);
  auto* right = t.FindCellByIdMutable(2);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  left->load = 0.8f;
  right->load = 0.2f;

  const float before_right_min_x = right->bounds.min_x;  // 0.f
  t.Balance(/*safety_bound=*/0.9f);
  EXPECT_GT(right->bounds.min_x, before_right_min_x);
  // Left upper bound and right lower bound stay coupled.
  EXPECT_FLOAT_EQ(left->bounds.max_x, right->bounds.min_x);
}

TEST(BSPTree, Balance_AggressionIncreasesOnRepeatedSameDirection) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());
  t.FindCellByIdMutable(1)->load = 0.9f;
  t.FindCellByIdMutable(2)->load = 0.1f;

  // After first balance direction is set but aggression is still 1.1 (since
  // prev_direction was kNone). The same-direction branch bumps aggression
  // on every subsequent pass.
  t.Balance(0.95f);
  const auto* root = dynamic_cast<const BSPInternal*>(t.Root());
  ASSERT_NE(root, nullptr);
  const float agg_after_first = root->AggressionForTest();

  t.Balance(0.95f);
  EXPECT_GT(root->AggressionForTest(), agg_after_first);
}

TEST(BSPTree, Balance_AggressionDecaysOnDirectionReversal) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());

  // First push left → right several times.
  t.FindCellByIdMutable(1)->load = 0.9f;
  t.FindCellByIdMutable(2)->load = 0.1f;
  t.Balance(0.95f);
  t.Balance(0.95f);
  t.Balance(0.95f);
  const auto* root = dynamic_cast<const BSPInternal*>(t.Root());
  ASSERT_NE(root, nullptr);
  const float agg_before_reversal = root->AggressionForTest();

  // Now flip the imbalance so next Balance shifts toward Left.
  t.FindCellByIdMutable(1)->load = 0.1f;
  t.FindCellByIdMutable(2)->load = 0.9f;
  t.Balance(0.95f);
  EXPECT_LT(root->AggressionForTest(), agg_before_reversal);
}

TEST(BSPTree, Balance_SafetyBoundHoldsStillWhenGrowingSideOverloaded) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());

  // Left heavier, but right (the side that would grow) is already at 0.95.
  // With safety_bound = 0.9 the balancer refuses to push.
  t.FindCellByIdMutable(1)->load = 0.99f;
  t.FindCellByIdMutable(2)->load = 0.95f;
  const float before = t.FindCellByIdMutable(2)->bounds.min_x;
  t.Balance(0.9f);
  EXPECT_FLOAT_EQ(t.FindCellByIdMutable(2)->bounds.min_x, before);
}

TEST(BSPTree, Balance_EqualLoadsDoNothing) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());
  t.FindCellByIdMutable(1)->load = 0.5f;
  t.FindCellByIdMutable(2)->load = 0.5f;
  const float before = t.FindCellByIdMutable(2)->bounds.min_x;
  t.Balance(0.95f);
  EXPECT_FLOAT_EQ(t.FindCellByIdMutable(2)->bounds.min_x, before);
}

TEST(BSPTree, Balance_ClampedToFiniteSubBoundsWhenSplitInsideNested) {
  // After splitting the right side into a nested internal, the inner
  // Balance must not push its split line outside its parent's sub-bounds.
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());
  ASSERT_TRUE(t.Split(2, BSPAxis::kX, 50.f, MakeLeafInfo(3, 30003)).HasValue());

  // Make cell 2's side extremely heavy to drive the inner split right.
  t.FindCellByIdMutable(2)->load = 100.f;
  t.FindCellByIdMutable(3)->load = 0.f;

  // Balance many times; the inner split line should never push cell 3's
  // min_x to or past the outer max_x (which is ±inf here — so instead we
  // just assert the bounds remain coherent and cell 3's min_x stays > 0).
  for (int i = 0; i < 10; ++i) t.Balance(1.0f);
  const auto* c2 = t.FindCellById(2);
  const auto* c3 = t.FindCellById(3);
  ASSERT_NE(c2, nullptr);
  ASSERT_NE(c3, nullptr);
  EXPECT_GT(c3->bounds.min_x, 0.f);
  EXPECT_FLOAT_EQ(c2->bounds.max_x, c3->bounds.min_x);
}

// ─── Serialize / Deserialize ────────────────────────────────────────────────

TEST(BSPTree, Serialize_RoundTrip_EmptyTree) {
  BSPTree t;
  BinaryWriter w;
  t.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = BSPTree::Deserialize(r);
  ASSERT_TRUE(rt.HasValue());
  EXPECT_TRUE(rt->Empty());
}

TEST(BSPTree, Serialize_RoundTrip_SingleLeaf) {
  auto t = MakeSingleCellTree(42, 31337);
  BinaryWriter w;
  t.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = BSPTree::Deserialize(r);
  ASSERT_TRUE(rt.HasValue());
  const auto* c = rt->FindCell(0.f, 0.f);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->cell_id, 42u);
  EXPECT_EQ(c->cellapp_addr.Port(), 31337u);
}

TEST(BSPTree, Serialize_RoundTrip_MultiLevelTree_PreservesStructureAndRouting) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());
  ASSERT_TRUE(t.Split(2, BSPAxis::kZ, 0.f, MakeLeafInfo(3, 30003)).HasValue());

  // Mutate loads — they must NOT survive the round trip (§9.6 Q7).
  t.FindCellByIdMutable(1)->load = 0.5f;
  t.FindCellByIdMutable(2)->load = 0.9f;

  BinaryWriter w;
  t.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = BSPTree::Deserialize(r);
  ASSERT_TRUE(rt.HasValue());

  EXPECT_EQ(rt->FindCell(-10.f, -10.f)->cell_id, 1u);
  EXPECT_EQ(rt->FindCell(10.f, -5.f)->cell_id, 2u);
  EXPECT_EQ(rt->FindCell(10.f, 5.f)->cell_id, 3u);

  // Loads should be zero on the receiver — it learns them via InformCellLoad.
  for (const auto* ci : rt->Leaves()) {
    EXPECT_FLOAT_EQ(ci->load, 0.f);
  }
}

TEST(BSPTree, Serialize_RoundTrip_LeafBoundsDerivable) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, -25.f, MakeLeafInfo(2, 30002)).HasValue());

  BinaryWriter w;
  t.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = BSPTree::Deserialize(r);
  ASSERT_TRUE(rt.HasValue());

  // Bounds aren't wire-serialised but are re-derived by PropagateBounds.
  const auto* left = rt->FindCellById(1);
  const auto* right = rt->FindCellById(2);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_FLOAT_EQ(left->bounds.max_x, -25.f);
  EXPECT_FLOAT_EQ(right->bounds.min_x, -25.f);
}

// ─── Leaves collector ───────────────────────────────────────────────────────

TEST(BSPTree, Leaves_ReturnsAllInOrder) {
  auto t = MakeSingleCellTree(1, 30001);
  ASSERT_TRUE(t.Split(1, BSPAxis::kX, 0.f, MakeLeafInfo(2, 30002)).HasValue());
  ASSERT_TRUE(t.Split(2, BSPAxis::kZ, 0.f, MakeLeafInfo(3, 30003)).HasValue());

  auto leaves = t.Leaves();
  ASSERT_EQ(leaves.size(), 3u);
  // Pre-order: left first, then right subtree (left-first).
  EXPECT_EQ(leaves[0]->cell_id, 1u);
  EXPECT_EQ(leaves[1]->cell_id, 2u);
  EXPECT_EQ(leaves[2]->cell_id, 3u);
}
