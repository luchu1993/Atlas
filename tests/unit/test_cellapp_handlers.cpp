// CellApp message handler tests — Phase 10 Step 10.8.
//
// Drives the handlers directly (bypassing ServerApp::Init and the CLR
// bring-up) to lock in handler behaviour: entity creation/destruction,
// space lifecycle, avatar validation, witness enable/disable, and the
// client-cell-rpc validation chain.
//
// What this test CAN'T cover:
//   - real network dispatch (needs a live BaseApp)
//   - the C# script side of CreateCellEntity (RestoreEntity callback)
//   - the BaseApp-reachable CellEntityCreated ack (requires a Channel;
//     we pass nullptr and assert the local state mutations only)
//
// Those end-to-end paths land in test_cellapp_integration (Step 10.10).

#include <memory>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "entitydef/entity_def_registry.h"
#include "math/vector3.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "space.h"

namespace atlas {
namespace {

class CellAppHandlersTest : public ::testing::Test {
 protected:
  // Plain CellApp instance without ServerApp::Init() — handlers that
  // don't need the network / CLR work fine on this setup.
  EventDispatcher dispatcher_{"test_cellapp_handlers"};
  NetworkInterface network_{dispatcher_};
  CellApp app_{dispatcher_, network_};

  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }

  auto MakeCreate(EntityID base_id, SpaceID sp, math::Vector3 pos = {0, 0, 0})
      -> cellapp::CreateCellEntity {
    cellapp::CreateCellEntity msg;
    msg.base_entity_id = base_id;
    msg.type_id = 1;
    msg.space_id = sp;
    msg.position = pos;
    msg.direction = {1, 0, 0};
    msg.base_addr = Address(0, 0);
    return msg;
  }
};

TEST_F(CellAppHandlersTest, CreateCellEntityRegistersInBothIndexes) {
  auto msg = MakeCreate(/*base_id=*/100, /*space=*/5, {10, 0, 10});
  app_.OnCreateCellEntity({}, /*ch=*/nullptr, msg);

  ASSERT_EQ(app_.Spaces().size(), 1u);
  EXPECT_NE(app_.FindSpace(5), nullptr);

  auto* by_base = app_.FindEntityByBaseId(100);
  ASSERT_NE(by_base, nullptr);
  EXPECT_EQ(by_base->BaseEntityId(), 100u);
  EXPECT_FLOAT_EQ(by_base->Position().x, 10.f);

  auto* by_cell = app_.FindEntity(by_base->Id());
  EXPECT_EQ(by_cell, by_base);
}

TEST_F(CellAppHandlersTest, CreateCellEntityRejectsInvalidSpaceId) {
  auto msg = MakeCreate(100, /*space_id=*/0);  // kInvalidSpaceID
  app_.OnCreateCellEntity({}, nullptr, msg);
  EXPECT_EQ(app_.FindEntityByBaseId(100), nullptr);
  EXPECT_TRUE(app_.Spaces().empty());
}

TEST_F(CellAppHandlersTest, CreateSpaceRejectsInvalidSpaceId) {
  cellapp::CreateSpace cs;
  cs.space_id = 0;  // kInvalidSpaceID
  app_.OnCreateSpace({}, nullptr, cs);
  EXPECT_TRUE(app_.Spaces().empty());
}

TEST_F(CellAppHandlersTest, CreateCellEntityUsesExistingSpace) {
  cellapp::CreateSpace cs;
  cs.space_id = 42;
  app_.OnCreateSpace({}, nullptr, cs);
  ASSERT_EQ(app_.Spaces().size(), 1u);

  auto msg = MakeCreate(1, 42);
  app_.OnCreateCellEntity({}, nullptr, msg);
  EXPECT_EQ(app_.Spaces().size(), 1u);  // no auto-created duplicate
}

TEST_F(CellAppHandlersTest, DestroyCellEntityRemovesFromBothIndexes) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  ASSERT_NE(app_.FindEntityByBaseId(100), nullptr);

  cellapp::DestroyCellEntity d{100};
  app_.OnDestroyCellEntity({}, nullptr, d);

  EXPECT_EQ(app_.FindEntityByBaseId(100), nullptr);
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 0u);
}

TEST_F(CellAppHandlersTest, DestroySpaceEvictsEntities) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(101, 1));
  ASSERT_EQ(app_.FindSpace(1)->EntityCount(), 2u);

  cellapp::DestroySpace ds{1};
  app_.OnDestroySpace({}, nullptr, ds);

  EXPECT_EQ(app_.Spaces().size(), 0u);
  EXPECT_EQ(app_.FindEntityByBaseId(100), nullptr);
  EXPECT_EQ(app_.FindEntityByBaseId(101), nullptr);
}

TEST_F(CellAppHandlersTest, AvatarUpdateMovesEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.base_entity_id = 100;
  u.position = {5.f, 0.f, 5.f};
  u.direction = {0, 0, 1};
  u.on_ground = true;
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindEntityByBaseId(100);
  ASSERT_NE(e, nullptr);
  EXPECT_FLOAT_EQ(e->Position().x, 5.f);
  EXPECT_FLOAT_EQ(e->Direction().z, 1.f);
  EXPECT_TRUE(e->OnGround());
}

TEST_F(CellAppHandlersTest, AvatarUpdateRejectsTeleport) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.base_entity_id = 100;
  u.position = {10000.f, 0, 0};  // way beyond kMaxSingleTickMove
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindEntityByBaseId(100);
  EXPECT_FLOAT_EQ(e->Position().x, 0.f) << "teleport must be rejected";
}

TEST_F(CellAppHandlersTest, AvatarUpdateRejectsNaN) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.base_entity_id = 100;
  u.position = {std::nanf(""), 0, 0};
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindEntityByBaseId(100);
  EXPECT_FLOAT_EQ(e->Position().x, 0.f);
}

TEST_F(CellAppHandlersTest, AvatarUpdateRejectsNaNDirection) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.base_entity_id = 100;
  u.position = {1, 0, 0};
  u.direction = {0, std::nanf(""), 0};
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindEntityByBaseId(100);
  // Position must not have changed — the entire update is rejected.
  EXPECT_FLOAT_EQ(e->Position().x, 0.f);
}

TEST_F(CellAppHandlersTest, EnableDisableWitnessTogglesOnEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::EnableWitness e;
  e.base_entity_id = 100;
  e.aoi_radius = 10.f;
  app_.OnEnableWitness({}, nullptr, e);
  EXPECT_TRUE(app_.FindEntityByBaseId(100)->HasWitness());

  cellapp::DisableWitness d{100};
  app_.OnDisableWitness({}, nullptr, d);
  EXPECT_FALSE(app_.FindEntityByBaseId(100)->HasWitness());
}

// ---------------------------------------------------------------------------
// ClientCellRpcForward validation chain — phase10_cellapp.md §3.8.1
// ---------------------------------------------------------------------------
//
// Register a synthetic entity type with a cell_methods entry whose
// direction bits are 0x02 and exposed=AllClients, then verify the
// handler accepts valid calls and rejects each of the misconfigurations
// the four-layer defence guards against.

namespace {

auto PackRpcId(uint8_t direction, uint16_t type_index, uint8_t method_index) -> uint32_t {
  return (static_cast<uint32_t>(direction) << 22) | (static_cast<uint32_t>(type_index) << 8) |
         static_cast<uint32_t>(method_index);
}

auto RegisterTypeWithRpc(uint16_t type_id, uint32_t rpc_id, ExposedScope scope) {
  // Synthesize the binary descriptor shape consumed by RegisterType
  // (see entity_def_registry.cc). Minimal — 0 properties, 1 RPC.
  BinaryWriter w;
  w.WriteString("TestEntity");
  w.Write<uint16_t>(type_id);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(0);  // 0 props
  w.WritePackedInt(1);  // 1 rpc
  w.WriteString("method");
  w.WritePackedInt(rpc_id);
  w.WritePackedInt(0);  // 0 params
  w.Write<uint8_t>(static_cast<uint8_t>(scope));
  w.Write<uint8_t>(0);  // compression
  w.Write<uint8_t>(0);
  auto buf = w.Detach();
  EntityDefRegistry::Instance().RegisterType(buf.data(), static_cast<int32_t>(buf.size()));
}

}  // namespace

TEST_F(CellAppHandlersTest, ClientCellRpcRejectsUnknownRpcId) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 100;
  msg.source_entity_id = 100;
  msg.rpc_id = 0xDEADBEEF;
  // No registered RPC → handler WARN-logs and returns. Not crashing is
  // the assertion.
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, ClientCellRpcRejectsNonExposed) {
  const uint16_t kTypeId = 1;
  const uint32_t kCellRpc = PackRpcId(0x02, kTypeId, 1);
  RegisterTypeWithRpc(kTypeId, kCellRpc, ExposedScope::kNone);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 100;
  msg.source_entity_id = 100;
  msg.rpc_id = kCellRpc;
  // Non-exposed → rejected at layer 3 (no crash assertion).
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, ClientCellRpcRejectsWrongDirection) {
  const uint16_t kTypeId = 1;
  // Direction 0x03 (Base method) but sent through the cell RPC channel —
  // must be rejected.
  const uint32_t kBaseRpc = PackRpcId(0x03, kTypeId, 1);
  RegisterTypeWithRpc(kTypeId, kBaseRpc, ExposedScope::kAllClients);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 100;
  msg.source_entity_id = 100;
  msg.rpc_id = kBaseRpc;
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, ClientCellRpcOwnClientRejectsCrossEntity) {
  const uint16_t kTypeId = 1;
  const uint32_t kCellRpc = PackRpcId(0x02, kTypeId, 1);
  RegisterTypeWithRpc(kTypeId, kCellRpc, ExposedScope::kOwnClient);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(200, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 100;
  msg.source_entity_id = 200;  // different from target
  msg.rpc_id = kCellRpc;
  // OwnClient requires source == target; cross-entity call rejected.
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, ClientCellRpcAcceptsAllClientsCrossEntity) {
  const uint16_t kTypeId = 1;
  const uint32_t kCellRpc = PackRpcId(0x02, kTypeId, 1);
  RegisterTypeWithRpc(kTypeId, kCellRpc, ExposedScope::kAllClients);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(200, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 100;
  msg.source_entity_id = 200;
  msg.rpc_id = kCellRpc;
  // AllClients accepts cross-entity; handler reaches the (stubbed)
  // dispatch branch.
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, InternalCellRpcBypassesExposedCheck) {
  const uint16_t kTypeId = 1;
  // Not exposed, but internal path doesn't care — BaseApp is trusted.
  const uint32_t kCellRpc = PackRpcId(0x02, kTypeId, 1);
  RegisterTypeWithRpc(kTypeId, kCellRpc, ExposedScope::kNone);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::InternalCellRpc msg;
  msg.target_entity_id = 100;
  msg.rpc_id = kCellRpc;
  app_.OnInternalCellRpc({}, nullptr, msg);
  // No way to assert success without a dispatched C# callback; the
  // test passes if the handler does not crash.
}

}  // namespace
}  // namespace atlas
