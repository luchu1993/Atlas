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

#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "entitydef/entity_def_registry.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "network/channel.h"
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

  void SetUp() override {
    EntityDefRegistry::Instance().clear();
    // C7 trust boundary: tests drive OnClientCellRpcForward with a
    // default-constructed Address{} as src (no wire dispatcher in the
    // loop). Trust that sentinel so the rest of the validation
    // pipeline — the piece these tests actually exercise — isn't
    // short-circuited at the trust check.
    app_.InsertTrustedBaseAppForTest(Address{});
  }
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

  // After C2: EnableWitness carries only the entity id; radius +
  // hysteresis come from CellAppConfig.
  cellapp::EnableWitness e;
  e.base_entity_id = 100;
  app_.OnEnableWitness({}, nullptr, e);
  EXPECT_TRUE(app_.FindEntityByBaseId(100)->HasWitness());

  cellapp::DisableWitness d{100};
  app_.OnDisableWitness({}, nullptr, d);
  EXPECT_FALSE(app_.FindEntityByBaseId(100)->HasWitness());
}

// OnCreateCellEntity no longer auto-enables a witness — witnesses are
// attached via the client-bind path (C3 hooks) or an explicit
// EnableWitness message. This test locks in the new contract.
TEST_F(CellAppHandlersTest, CreateCellEntityDoesNotAutoEnableWitness) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  auto* entity = app_.FindEntityByBaseId(100);
  ASSERT_NE(entity, nullptr);
  EXPECT_FALSE(entity->HasWitness());
}

TEST_F(CellAppHandlersTest, OnSetAoIRadiusUpdatesActiveWitness) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  cellapp::EnableWitness e{100};
  app_.OnEnableWitness({}, nullptr, e);
  auto* entity = app_.FindEntityByBaseId(100);
  ASSERT_TRUE(entity->HasWitness());

  cellapp::SetAoIRadius s;
  s.base_entity_id = 100;
  s.radius = 42.5f;
  s.hysteresis = 7.f;
  app_.OnSetAoIRadius({}, nullptr, s);

  EXPECT_FLOAT_EQ(entity->GetWitness()->AoIRadius(), 42.5f);
  EXPECT_FLOAT_EQ(entity->GetWitness()->Hysteresis(), 7.f);
}

TEST_F(CellAppHandlersTest, OnSetAoIRadiusMissingWitnessIsNoop) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  // No EnableWitness — witness is absent.
  cellapp::SetAoIRadius s;
  s.base_entity_id = 100;
  s.radius = 42.5f;
  s.hysteresis = 7.f;
  app_.OnSetAoIRadius({}, nullptr, s);  // should log-warn, not crash

  EXPECT_FALSE(app_.FindEntityByBaseId(100)->HasWitness());
}

// Phase 11 C1: CellApp reports NumRealEntities to CellAppMgr in every
// InformCellLoad. Ghosts MUST NOT count — the balancer tracks Real load.
TEST_F(CellAppHandlersTest, NumRealEntitiesExcludesGhosts) {
  EXPECT_EQ(app_.NumRealEntities(), 0u);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(101, 1));
  EXPECT_EQ(app_.NumRealEntities(), 2u);

  // Convert one to a Ghost via the Phase 11 path. A Ghost no longer
  // counts as Real-on-this-CellApp.
  auto* ent = app_.FindEntityByBaseId(100);
  ASSERT_NE(ent, nullptr);
  ent->ConvertRealToGhost(/*new_real_channel=*/nullptr);
  EXPECT_EQ(app_.NumRealEntities(), 1u);
}

// Phase 11 C1: persistent_load_ initial state is 0 until at least one
// tick has driven UpdatePersistentLoad. Handler-level tests never call
// AdvanceTime, so this is the stable observable state.
TEST_F(CellAppHandlersTest, PersistentLoadStartsAtZero) {
  EXPECT_FLOAT_EQ(app_.PersistentLoad(), 0.f);
}

TEST_F(CellAppHandlersTest, OnSetAoIRadiusClampsToMax) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  cellapp::EnableWitness e{100};
  app_.OnEnableWitness({}, nullptr, e);

  // CellAppConfig::MaxAoIRadius() defaults to 500m. A value beyond that
  // must be clamped inside Witness::SetAoIRadius.
  cellapp::SetAoIRadius s;
  s.base_entity_id = 100;
  s.radius = 10'000.f;
  s.hysteresis = 5.f;
  app_.OnSetAoIRadius({}, nullptr, s);

  EXPECT_FLOAT_EQ(app_.FindEntityByBaseId(100)->GetWitness()->AoIRadius(), 500.f);
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

// Phase 11 C7: baseline trust check. Any source address not registered
// via InsertTrustedBaseAppForTest (or the machined Birth subscription
// in production) gets its ClientCellRpcForward dropped at the front
// door — before any L3/L4 validation or script dispatch.
TEST_F(CellAppHandlersTest, ClientCellRpcRejectsUntrustedSource) {
  const uint32_t kCellRpc = 0x00800101u;  // direction=0x02, type=1, method=1
  BinaryWriter w;
  w.WriteString("TestEntity");
  w.Write<uint16_t>(1);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(0);
  w.WritePackedInt(1);
  w.WriteString("method");
  w.WritePackedInt(kCellRpc);
  w.WritePackedInt(0);
  w.Write<uint8_t>(static_cast<uint8_t>(ExposedScope::kAllClients));
  w.Write<uint8_t>(0);
  w.Write<uint8_t>(0);
  auto buf = w.Detach();
  EntityDefRegistry::Instance().RegisterType(buf.data(), static_cast<int32_t>(buf.size()));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(500, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 500;
  msg.source_entity_id = 500;
  msg.rpc_id = kCellRpc;

  // SetUp seeded trust for Address{}; use a different (untrusted) addr.
  const Address untrusted(0x7F000001u, 12345);
  app_.OnClientCellRpcForward(untrusted, nullptr, msg);
  // No direct observable — the handler drops silently with a warn log
  // rather than throwing. Defensive "didn't crash" assertion.
  SUCCEED();

  // Positive control: same msg through the trusted Address{} passes
  // the trust check and reaches downstream validation (which accepts).
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

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

// ---------------------------------------------------------------------------
// Phase 11 inter-CellApp handler coverage
// ---------------------------------------------------------------------------

namespace {

auto FakeChannel(uintptr_t tag) -> Channel* {
  return reinterpret_cast<Channel*>(tag);
}

}  // namespace

TEST_F(CellAppHandlersTest, CreateGhostWithNullChannelRejected) {
  // Pre-create the space so the handler doesn't bail for a missing space.
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  cellapp::CreateGhost msg;
  msg.real_entity_id = 500;
  msg.type_id = 1;
  msg.space_id = 1;
  msg.position = {0, 0, 0};
  msg.direction = {1, 0, 0};
  msg.on_ground = false;
  msg.real_cellapp_addr = Address(0x7F000001u, 30001);
  msg.base_addr = Address(0x7F000001u, 20000);
  msg.base_entity_id = 500;
  msg.event_seq = 0;
  msg.volatile_seq = 0;

  app_.OnCreateGhost({}, /*ch=*/nullptr, msg);

  // Null channel must cause the handler to bail out — no ghost created.
  EXPECT_EQ(app_.FindEntity(500), nullptr);
}

TEST_F(CellAppHandlersTest, GhostPositionUpdateRejectsNaN) {
  // Create a space for the ghost to live in.
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  // Create a ghost via the handler with a non-null fake channel.
  cellapp::CreateGhost cg;
  cg.real_entity_id = 600;
  cg.type_id = 1;
  cg.space_id = 1;
  cg.position = {10, 0, 20};
  cg.direction = {1, 0, 0};
  cg.on_ground = false;
  cg.real_cellapp_addr = Address(0x7F000001u, 30001);
  cg.base_addr = Address(0x7F000001u, 20000);
  cg.base_entity_id = 600;
  cg.event_seq = 0;
  cg.volatile_seq = 0;

  app_.OnCreateGhost({}, FakeChannel(0xBEEF), cg);

  auto* ghost = app_.FindEntity(600);
  ASSERT_NE(ghost, nullptr);
  EXPECT_FLOAT_EQ(ghost->Position().x, 10.f);
  EXPECT_FLOAT_EQ(ghost->Position().z, 20.f);

  // Send a position update containing NaN — must be rejected.
  cellapp::GhostPositionUpdate upd;
  upd.ghost_entity_id = 600;
  upd.position = {std::nanf(""), 0, 0};
  upd.direction = {1, 0, 0};
  upd.on_ground = true;
  upd.volatile_seq = 1;
  app_.OnGhostPositionUpdate({}, nullptr, upd);

  // Position must remain at the original values.
  EXPECT_FLOAT_EQ(ghost->Position().x, 10.f);
  EXPECT_FLOAT_EQ(ghost->Position().z, 20.f);
}

}  // namespace
}  // namespace atlas
