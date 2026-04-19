// CellAppNativeProvider tests — Phase 10 Step 10.7.
//
// Covers the C# → C++ NativeApi surface the CellApp process installs:
//   - SetEntityPosition propagates into CellEntity + RangeList
//   - PublishReplicationFrame forwards event/volatile seqs with correct
//     snapshot and delta spans
//   - AddMoveController / AddTimerController / AddProximityController
//     install live controllers owned by the entity
//   - CancelController tears them back down
//   - unknown entity_ids log-and-skip rather than crash

#include <cstddef>
#include <memory>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "cellapp_native_provider.h"
#include "math/vector3.h"
#include "server/server_config.h"
#include "space.h"

namespace atlas {
namespace {

class CellAppNativeProviderTest : public ::testing::Test {
 protected:
  Space space_{1};
  CellAppNativeProvider provider_{[this](uint32_t id) { return space_.FindEntity(id); }};

  auto AddEntity(EntityID id, math::Vector3 pos = {0, 0, 0}) -> CellEntity* {
    return space_.AddEntity(
        std::make_unique<CellEntity>(id, 1, space_, pos, math::Vector3{1, 0, 0}));
  }
};

TEST_F(CellAppNativeProviderTest, SetPositionUpdatesEntity) {
  auto* e = AddEntity(10);
  provider_.SetEntityPosition(10, 5.f, 0.f, 7.f);
  EXPECT_FLOAT_EQ(e->Position().x, 5.f);
  EXPECT_FLOAT_EQ(e->Position().z, 7.f);
  EXPECT_FLOAT_EQ(e->RangeNode().X(), 5.f);
}

TEST_F(CellAppNativeProviderTest, SetPositionUnknownEntityIsSafe) {
  provider_.SetEntityPosition(9999, 1.f, 2.f, 3.f);
  // Just verifies we don't crash. The warning log is fire-and-forget.
  EXPECT_EQ(space_.EntityCount(), 0u);
}

TEST_F(CellAppNativeProviderTest, PublishReplicationFrameAdvancesSeqsAndCopiesBuffers) {
  auto* e = AddEntity(10);

  const std::byte owner_snap[] = {std::byte{0x11}, std::byte{0x22}};
  const std::byte other_snap[] = {std::byte{0x33}};
  const std::byte owner_delta[] = {std::byte{0xA1}};
  const std::byte other_delta[] = {std::byte{0xB1}, std::byte{0xB2}};

  provider_.PublishReplicationFrame(10, /*event_seq=*/1, /*volatile_seq=*/1, owner_snap,
                                    static_cast<int32_t>(sizeof(owner_snap)), other_snap,
                                    static_cast<int32_t>(sizeof(other_snap)), owner_delta,
                                    static_cast<int32_t>(sizeof(owner_delta)), other_delta,
                                    static_cast<int32_t>(sizeof(other_delta)));

  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, 1u);
  EXPECT_EQ(state->latest_volatile_seq, 1u);
  ASSERT_EQ(state->owner_snapshot.size(), 2u);
  EXPECT_EQ(state->owner_snapshot[0], std::byte{0x11});
  ASSERT_EQ(state->history.size(), 1u);
  EXPECT_EQ(state->history.front().owner_delta.size(), 1u);
  EXPECT_EQ(state->history.front().other_delta.size(), 2u);
}

TEST_F(CellAppNativeProviderTest, PublishReplicationFrameWithEmptyBuffersIsSafe) {
  auto* e = AddEntity(10);
  // volatile-only frame — no snapshots or deltas needed.
  provider_.PublishReplicationFrame(10, /*event=*/0, /*volatile=*/1, nullptr, 0, nullptr, 0,
                                    nullptr, 0, nullptr, 0);
  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, 0u);
  EXPECT_EQ(state->latest_volatile_seq, 1u);
}

TEST_F(CellAppNativeProviderTest, AddMoveControllerRegistersAndAdvances) {
  auto* e = AddEntity(10);
  auto id = provider_.AddMoveController(10, /*dest=*/20.f, 0.f, 0.f, /*speed=*/100.f,
                                        /*user_arg=*/7);
  EXPECT_GT(id, 0);
  EXPECT_TRUE(e->GetControllers().Contains(static_cast<ControllerID>(id)));

  // Tick drives movement; with speed=100 and dest at (20,0,0), one tick
  // of 0.3s covers the full distance and the controller finishes.
  e->GetControllers().Update(0.3f);
  EXPECT_FLOAT_EQ(e->Position().x, 20.f);
  EXPECT_FALSE(e->GetControllers().Contains(static_cast<ControllerID>(id)));
}

TEST_F(CellAppNativeProviderTest, AddTimerControllerRegistersRepeating) {
  auto* e = AddEntity(10);
  auto id = provider_.AddTimerController(10, /*interval=*/0.1f, /*repeat=*/true,
                                         /*user_arg=*/0);
  EXPECT_GT(id, 0);
  EXPECT_TRUE(e->GetControllers().Contains(static_cast<ControllerID>(id)));
  // Repeating timer stays alive after firing.
  e->GetControllers().Update(0.1f);
  EXPECT_TRUE(e->GetControllers().Contains(static_cast<ControllerID>(id)));
}

TEST_F(CellAppNativeProviderTest, AddProximityControllerIsFunctional) {
  auto* e = AddEntity(10);
  auto id = provider_.AddProximityController(10, /*range=*/5.f, /*user_arg=*/0);
  EXPECT_GT(id, 0);
  EXPECT_TRUE(e->GetControllers().Contains(static_cast<ControllerID>(id)));
}

TEST_F(CellAppNativeProviderTest, CancelControllerRemoves) {
  auto* e = AddEntity(10);
  auto id = provider_.AddTimerController(10, 10.f, true, 0);
  EXPECT_TRUE(e->GetControllers().Contains(static_cast<ControllerID>(id)));
  provider_.CancelController(10, id);
  EXPECT_FALSE(e->GetControllers().Contains(static_cast<ControllerID>(id)));
}

TEST_F(CellAppNativeProviderTest, UnknownEntityInControllerApisIsSafe) {
  EXPECT_EQ(provider_.AddMoveController(999, 0, 0, 0, 1.f, 0), 0);
  EXPECT_EQ(provider_.AddTimerController(999, 1.f, false, 0), 0);
  EXPECT_EQ(provider_.AddProximityController(999, 5.f, 0), 0);
  provider_.CancelController(999, 42);  // must not crash
}

TEST_F(CellAppNativeProviderTest, ProcessPrefixIsCellApp) {
  EXPECT_EQ(provider_.GetProcessPrefix(), static_cast<uint8_t>(ProcessType::kCellApp));
}

// ---- SetNativeCallbacks tests ----

// Helper: build a minimal callback table with known sentinel function ptrs.
namespace {

bool g_restore_called = false;
uint32_t g_restore_entity_id = 0;
uint16_t g_restore_type_id = 0;
void FakeRestore(uint32_t eid, uint16_t tid, int64_t, const uint8_t*, int32_t) {
  g_restore_called = true;
  g_restore_entity_id = eid;
  g_restore_type_id = tid;
}

bool g_dispatch_called = false;
uint32_t g_dispatch_entity_id = 0;
uint32_t g_dispatch_rpc_id = 0;
void FakeDispatch(uint32_t eid, uint32_t rid, const uint8_t*, int32_t) {
  g_dispatch_called = true;
  g_dispatch_entity_id = eid;
  g_dispatch_rpc_id = rid;
}

bool g_destroyed_called = false;
uint32_t g_destroyed_entity_id = 0;
void FakeDestroyed(uint32_t eid) {
  g_destroyed_called = true;
  g_destroyed_entity_id = eid;
}

// Matches the packed table C# would send (see NativeCallbackTable layout).
#pragma pack(push, 1)
struct TestCallbackTable {
  void* restore;
  void* get_entity_data;
  void* entity_destroyed;
  void* dispatch_rpc;
};
#pragma pack(pop)

}  // namespace

TEST_F(CellAppNativeProviderTest, SetNativeCallbacksStoresCallbacks) {
  TestCallbackTable table{};
  table.restore = reinterpret_cast<void*>(&FakeRestore);
  table.dispatch_rpc = reinterpret_cast<void*>(&FakeDispatch);
  table.entity_destroyed = reinterpret_cast<void*>(&FakeDestroyed);

  provider_.SetNativeCallbacks(&table, sizeof(table));

  EXPECT_NE(provider_.restore_entity_fn(), nullptr);
  EXPECT_NE(provider_.dispatch_rpc_fn(), nullptr);
  EXPECT_NE(provider_.entity_destroyed_fn(), nullptr);
}

TEST_F(CellAppNativeProviderTest, SetNativeCallbacksRejectsTooSmallTable) {
  int32_t dummy = 42;
  provider_.SetNativeCallbacks(&dummy, sizeof(dummy));
  EXPECT_EQ(provider_.restore_entity_fn(), nullptr);
  EXPECT_EQ(provider_.dispatch_rpc_fn(), nullptr);
}

TEST_F(CellAppNativeProviderTest, SetNativeCallbacksRestoreRoundtrips) {
  g_restore_called = false;
  TestCallbackTable table{};
  table.restore = reinterpret_cast<void*>(&FakeRestore);
  table.dispatch_rpc = reinterpret_cast<void*>(&FakeDispatch);
  table.entity_destroyed = reinterpret_cast<void*>(&FakeDestroyed);
  provider_.SetNativeCallbacks(&table, sizeof(table));

  provider_.restore_entity_fn()(42, 7, 0, nullptr, 0);
  EXPECT_TRUE(g_restore_called);
  EXPECT_EQ(g_restore_entity_id, 42u);
  EXPECT_EQ(g_restore_type_id, 7u);
}

TEST_F(CellAppNativeProviderTest, SetNativeCallbacksDispatchRoundtrips) {
  g_dispatch_called = false;
  TestCallbackTable table{};
  table.restore = reinterpret_cast<void*>(&FakeRestore);
  table.dispatch_rpc = reinterpret_cast<void*>(&FakeDispatch);
  table.entity_destroyed = reinterpret_cast<void*>(&FakeDestroyed);
  provider_.SetNativeCallbacks(&table, sizeof(table));

  provider_.dispatch_rpc_fn()(100, 0x00800001, nullptr, 0);
  EXPECT_TRUE(g_dispatch_called);
  EXPECT_EQ(g_dispatch_entity_id, 100u);
  EXPECT_EQ(g_dispatch_rpc_id, 0x00800001u);
}

// ============================================================================
// Phase 11 PR-3 — Ghost write guards
// ============================================================================
//
// Every write path that mutates Real-only state must log-and-skip when its
// entity is a Ghost. Resolution from Phase 11 Q2: soft guard, not a hard
// assert, so a misbehaving script can't take the CellApp down.

TEST_F(CellAppNativeProviderTest, SetPositionRejectedOnGhost) {
  auto* e = space_.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 500, 1, space_, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0},
      /*real_channel=*/reinterpret_cast<Channel*>(0xBEEF)));
  ASSERT_TRUE(e->IsGhost());
  provider_.SetEntityPosition(500, 9.f, 9.f, 9.f);
  // Position unchanged — guard blocked the write.
  EXPECT_FLOAT_EQ(e->Position().x, 0.f);
  EXPECT_FLOAT_EQ(e->Position().z, 0.f);
}

TEST_F(CellAppNativeProviderTest, PublishReplicationFrameRejectedOnGhost) {
  auto* e = space_.AddEntity(
      std::make_unique<CellEntity>(CellEntity::GhostTag{}, 501, 1, space_, math::Vector3{0, 0, 0},
                                   math::Vector3{1, 0, 0}, reinterpret_cast<Channel*>(0xBEEF)));
  provider_.PublishReplicationFrame(501, /*event_seq=*/1, /*volatile_seq=*/1, nullptr, 0, nullptr,
                                    0, nullptr, 0, nullptr, 0);
  // Ghost's replication_state_ was never allocated by the provider.
  EXPECT_EQ(e->GetReplicationState(), nullptr);
}

TEST_F(CellAppNativeProviderTest, AddControllersRejectedOnGhost) {
  auto* e = space_.AddEntity(
      std::make_unique<CellEntity>(CellEntity::GhostTag{}, 502, 1, space_, math::Vector3{0, 0, 0},
                                   math::Vector3{1, 0, 0}, reinterpret_cast<Channel*>(0xBEEF)));
  EXPECT_EQ(provider_.AddMoveController(502, 0, 0, 0, 1.f, 0), 0);
  EXPECT_EQ(provider_.AddTimerController(502, 1.f, false, 0), 0);
  EXPECT_EQ(provider_.AddProximityController(502, 5.f, 0), 0);
  // Nothing attached to the Ghost's controllers list.
  EXPECT_EQ(e->GetControllers().Count(), 0u);
}

}  // namespace
}  // namespace atlas
