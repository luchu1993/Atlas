#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "cellappmgr/bsp_tree.h"
#include "clrscript/native_api_provider.h"
#include "entitydef/entity_def_registry.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "real_entity_data.h"
#include "space.h"

namespace atlas {
namespace {

// Ghost-lifecycle tests inject these via SetNativeCallbacks; free funcs are
// the only way to satisfy the C-style native callback signatures.
struct GhostCall {
  enum Kind { kRestoreGhost, kDestroyGhost, kRestoreEntity, kEntityDestroyed, kMigratingOut };
  Kind kind;
  uint32_t entity_id;
  uint16_t type_id;
  int32_t snapshot_len;
};
std::vector<GhostCall>* g_ghost_calls = nullptr;

extern "C" void GhostTestRestoreGhost(uint32_t eid, uint16_t tid, const uint8_t*, int32_t len) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kRestoreGhost, eid, tid, len});
}
extern "C" void GhostTestDestroyGhost(uint32_t eid) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kDestroyGhost, eid, 0, 0});
}
extern "C" void GhostTestRestoreEntity(uint32_t eid, uint16_t tid, int64_t,
                                       const uint8_t*, int32_t) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kRestoreEntity, eid, tid, 0});
}
extern "C" void GhostTestEntityDestroyed(uint32_t eid) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kEntityDestroyed, eid, 0, 0});
}
extern "C" void GhostTestMigratingOut(uint32_t eid) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kMigratingOut, eid, 0, 0});
}

#pragma pack(push, 1)
struct GhostTestCallbackTable {
  void* restore_entity;
  void* get_entity_data;
  void* entity_destroyed;
  void* dispatch_rpc;
  void* get_owner_snapshot;
  void* serialize_entity;
  void* proximity_event;
  void* coro_on_rpc_complete;
  void* entity_lifecycle_cancel;
  void* timer_event;
  void* entity_migrating_out;
  void* restore_ghost;
  void* destroy_ghost;
};
#pragma pack(pop)

class CellAppHandlersTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test_cellapp_handlers"};
  NetworkInterface network_{dispatcher_};
  CellApp app_{dispatcher_, network_};
  // Keeps native_provider_ alive for the test duration; CreateNativeProvider
  // hands back ownership to the caller in production (ScriptApp owns it).
  std::unique_ptr<INativeApiProvider> native_provider_holder_;
  std::vector<GhostCall> ghost_calls_;

  void SetUp() override {
    EntityDefRegistry::Instance().clear();
    app_.InsertTrustedBaseAppForTest(Address{});
    g_ghost_calls = &ghost_calls_;
  }
  void TearDown() override {
    g_ghost_calls = nullptr;
    EntityDefRegistry::Instance().clear();
  }

  void EnableGhostLifecycleCallbacks() {
    native_provider_holder_ = app_.CreateNativeProviderForTest();
    GhostTestCallbackTable table{};
    table.restore_entity = reinterpret_cast<void*>(&GhostTestRestoreEntity);
    table.entity_destroyed = reinterpret_cast<void*>(&GhostTestEntityDestroyed);
    table.entity_migrating_out = reinterpret_cast<void*>(&GhostTestMigratingOut);
    table.restore_ghost = reinterpret_cast<void*>(&GhostTestRestoreGhost);
    table.destroy_ghost = reinterpret_cast<void*>(&GhostTestDestroyGhost);
    app_.NativeProvider()->SetNativeCallbacks(&table, sizeof(table));
  }

  auto MakeCreate(EntityID entity_id, SpaceID sp, math::Vector3 pos = {0, 0, 0})
      -> cellapp::CreateCellEntity {
    cellapp::CreateCellEntity msg;
    msg.entity_id = entity_id;
    msg.type_id = 1;
    msg.space_id = sp;
    msg.position = pos;
    msg.direction = {1, 0, 0};
    msg.base_addr = Address(0, 0);
    return msg;
  }

  auto MakeGhost(EntityID entity_id, math::Vector3 pos = {0, 0, 0})
      -> cellapp::CreateGhost {
    cellapp::CreateGhost msg;
    msg.entity_id = entity_id;
    msg.type_id = 1;
    msg.space_id = 1;
    msg.position = pos;
    msg.direction = {1, 0, 0};
    msg.real_cellapp_addr = Address(0x7F000001u, 26002);
    return msg;
  }
};

auto FakeChannel(uintptr_t tag) -> Channel* {
  return reinterpret_cast<Channel*>(tag);
}

TEST_F(CellAppHandlersTest, CreateCellEntityUsesUnifiedEntityId) {
  auto msg = MakeCreate(/*entity_id=*/100, /*space=*/5, {10, 0, 10});
  app_.OnCreateCellEntity({}, /*ch=*/nullptr, msg);

  ASSERT_EQ(app_.Spaces().size(), 1u);
  EXPECT_NE(app_.FindSpace(5), nullptr);

  auto* real_entity = app_.FindRealEntity(100);
  ASSERT_NE(real_entity, nullptr);
  EXPECT_EQ(real_entity->Id(), 100u);
  EXPECT_FLOAT_EQ(real_entity->Position().x, 10.f);

  auto* by_entity_id = app_.FindEntity(real_entity->Id());
  EXPECT_EQ(by_entity_id, real_entity);
}

TEST_F(CellAppHandlersTest, CreateCellEntityRejectsInvalidSpaceId) {
  auto msg = MakeCreate(100, /*space_id=*/0);
  app_.OnCreateCellEntity({}, nullptr, msg);
  EXPECT_EQ(app_.FindRealEntity(100), nullptr);
  EXPECT_TRUE(app_.Spaces().empty());
}

TEST_F(CellAppHandlersTest, CreateSpaceRejectsInvalidSpaceId) {
  cellapp::CreateSpace cs;
  cs.space_id = 0;
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
  EXPECT_EQ(app_.Spaces().size(), 1u);
}

TEST_F(CellAppHandlersTest, DuplicateCreateCellEntityKeepsExistingReal) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {1, 0, 1}));
  auto* original = app_.FindRealEntity(100);
  ASSERT_NE(original, nullptr);
  ASSERT_EQ(app_.FindSpace(1)->EntityCount(), 1u);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 2, {9, 0, 9}));

  EXPECT_EQ(app_.FindRealEntity(100), original);
  EXPECT_EQ(app_.FindEntity(100), original);
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 1u);
  EXPECT_EQ(app_.FindSpace(2), nullptr);
  EXPECT_FLOAT_EQ(original->Position().x, 1.f);
  EXPECT_FLOAT_EQ(original->Position().z, 1.f);
}

TEST_F(CellAppHandlersTest, DuplicateCreateCellEntityRejectsExistingGhost) {
  cellapp::CreateGhost ghost;
  ghost.entity_id = 200;
  ghost.type_id = 1;
  ghost.space_id = 1;
  ghost.position = {2, 0, 2};
  ghost.direction = {1, 0, 0};
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), ghost);

  auto* original = app_.FindEntity(200);
  ASSERT_NE(original, nullptr);
  ASSERT_TRUE(original->IsGhost());
  ASSERT_EQ(app_.FindSpace(1)->EntityCount(), 1u);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(200, 2, {9, 0, 9}));

  EXPECT_EQ(app_.FindEntity(200), original);
  EXPECT_EQ(app_.FindRealEntity(200), nullptr);
  EXPECT_TRUE(original->IsGhost());
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 1u);
  EXPECT_EQ(app_.FindSpace(2), nullptr);
}

TEST_F(CellAppHandlersTest, CreateGhostOnRealIsIdempotentNoOp) {
  // Stale CreateGhost arriving after the entity got promoted to Real here
  // (sender raced its own Offload across a channel reconnect) must be a
  // silent no-op, not an "id collision" error.
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(300, 1, {3, 0, 3}));
  auto* real = app_.FindRealEntity(300);
  ASSERT_NE(real, nullptr);
  const auto pos_before = real->Position();

  cellapp::CreateGhost stale;
  stale.entity_id = 300;
  stale.type_id = 1;
  stale.space_id = 1;
  stale.position = {99, 0, 99};
  stale.direction = {0, 0, 1};
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), stale);

  EXPECT_EQ(app_.FindRealEntity(300), real);
  EXPECT_TRUE(real->IsReal());
  EXPECT_FLOAT_EQ(real->Position().x, pos_before.x);
  EXPECT_FLOAT_EQ(real->Position().z, pos_before.z);
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 1u);
}

TEST_F(CellAppHandlersTest, DestroyCellEntityRemovesEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  ASSERT_NE(app_.FindRealEntity(100), nullptr);

  cellapp::DestroyCellEntity d{100};
  app_.OnDestroyCellEntity({}, nullptr, d);

  EXPECT_EQ(app_.FindRealEntity(100), nullptr);
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 0u);
}

TEST_F(CellAppHandlersTest, CreateLocalEntityMintsIdFromIDClient) {
  auto& id_client = const_cast<IDClient&>(app_.GetIdClientForTest());
  id_client.AddIds(500, 600);

  EntityID id = app_.CreateLocalEntity(/*type_id=*/1, /*space_id=*/1, {1, 0, 2}, {1, 0, 0},
                                       /*on_ground=*/false);
  ASSERT_NE(id, kInvalidEntityID);
  auto* real = app_.FindRealEntity(id);
  ASSERT_NE(real, nullptr);
  EXPECT_TRUE(real->IsLocal());
  EXPECT_FLOAT_EQ(real->Position().x, 1.f);
  EXPECT_FLOAT_EQ(real->Position().z, 2.f);
}

TEST_F(CellAppHandlersTest, CreateLocalEntityFailsWhenIDClientEmpty) {
  EntityID id = app_.CreateLocalEntity(1, 1, {0, 0, 0}, {1, 0, 0}, false);
  EXPECT_EQ(id, kInvalidEntityID);
}

// Reproducer for the cluster-mode "atlas_set_position: unknown entity_id"
// warning seen on every NPC tick: the NPC OnTick path goes
// `Owner.Position = ...` -> NativeApi.SetEntityPosition(entityId) which
// reaches CellAppNativeProvider::SetEntityPosition; that calls the
// lookup_ closure (bound to CellApp::FindEntity). If the closure cannot
// see the entity_population_ entry the production path will warn and
// drop the write. This test exercises that exact codepath in-process.
TEST_F(CellAppHandlersTest, NativeProviderSetPositionResolvesLocalEntity) {
  auto& id_client = const_cast<IDClient&>(app_.GetIdClientForTest());
  id_client.AddIds(500, 600);

  CellAppNativeProvider provider(
      [this](uint32_t id) { return app_.FindEntity(id); }, network_);

  EntityID id = app_.CreateLocalEntity(/*type_id=*/1, /*space_id=*/1, {0, 0, 0}, {1, 0, 0},
                                       /*on_ground=*/false);
  ASSERT_NE(id, kInvalidEntityID);

  provider.SetEntityPosition(id, 7.f, 0.f, 9.f);
  provider.SetEntityDirection(id, 0.f, 0.f, 1.f);

  auto* real = app_.FindRealEntity(id);
  ASSERT_NE(real, nullptr);
  EXPECT_FLOAT_EQ(real->Position().x, 7.f);
  EXPECT_FLOAT_EQ(real->Position().z, 9.f);
  EXPECT_FLOAT_EQ(real->Direction().z, 1.f);
}

TEST_F(CellAppHandlersTest, DestroyLocalEntityClearsLocalEntity) {
  auto& id_client = const_cast<IDClient&>(app_.GetIdClientForTest());
  id_client.AddIds(500, 600);

  EntityID id = app_.CreateLocalEntity(1, 1, {0, 0, 0}, {1, 0, 0}, false);
  ASSERT_NE(id, kInvalidEntityID);

  app_.DestroyLocalEntity(id);
  EXPECT_EQ(app_.FindRealEntity(id), nullptr);
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 0u);
}

TEST_F(CellAppHandlersTest, OffloadPreservesIsLocalFlag) {
  // is_local must survive Offload: an NPC created via CreateLocalCell on the
  // source cellapp would otherwise be refused on DestroySelf after migration.
  cellapp::OffloadEntity msg;
  msg.entity_id = 7777;
  msg.type_id = 1;
  msg.space_id = 1;
  msg.position = {1, 0, 1};
  msg.direction = {1, 0, 0};
  msg.is_local = true;

  app_.OnOffloadEntity({}, nullptr, msg);

  auto* real = app_.FindRealEntity(7777);
  ASSERT_NE(real, nullptr);
  EXPECT_TRUE(real->IsLocal());

  app_.DestroyLocalEntity(7777);
  EXPECT_EQ(app_.FindRealEntity(7777), nullptr);
}

TEST_F(CellAppHandlersTest, DestroyLocalEntityRefusesBaseOwnedEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(200, 1));
  auto* real = app_.FindRealEntity(200);
  ASSERT_NE(real, nullptr);
  ASSERT_FALSE(real->IsLocal());

  app_.DestroyLocalEntity(200);
  EXPECT_NE(app_.FindRealEntity(200), nullptr);
}

TEST_F(CellAppHandlersTest, DestroySpaceEvictsEntities) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(101, 1));
  ASSERT_EQ(app_.FindSpace(1)->EntityCount(), 2u);

  cellapp::DestroySpace ds{1};
  app_.OnDestroySpace({}, nullptr, ds);

  EXPECT_EQ(app_.Spaces().size(), 0u);
  EXPECT_EQ(app_.FindRealEntity(100), nullptr);
  EXPECT_EQ(app_.FindRealEntity(101), nullptr);
}

TEST_F(CellAppHandlersTest, AvatarUpdateMovesEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.entity_id = 100;
  u.position = {5.f, 0.f, 5.f};
  u.direction = {0, 0, 1};
  u.on_ground = true;
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindRealEntity(100);
  ASSERT_NE(e, nullptr);
  EXPECT_FLOAT_EQ(e->Position().x, 5.f);
  EXPECT_FLOAT_EQ(e->Direction().z, 1.f);
  EXPECT_TRUE(e->OnGround());
}

TEST_F(CellAppHandlersTest, AvatarUpdateRejectsTeleport) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.entity_id = 100;
  u.position = {10000.f, 0, 0};
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindRealEntity(100);
  EXPECT_FLOAT_EQ(e->Position().x, 0.f) << "teleport must be rejected";
}

TEST_F(CellAppHandlersTest, AvatarUpdateRejectsNaN) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.entity_id = 100;
  u.position = {std::nanf(""), 0, 0};
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindRealEntity(100);
  EXPECT_FLOAT_EQ(e->Position().x, 0.f);
}

TEST_F(CellAppHandlersTest, AvatarUpdateRejectsNaNDirection) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1, {0, 0, 0}));

  cellapp::AvatarUpdate u;
  u.entity_id = 100;
  u.position = {1, 0, 0};
  u.direction = {0, std::nanf(""), 0};
  app_.OnAvatarUpdate({}, nullptr, u);

  auto* e = app_.FindRealEntity(100);
  EXPECT_FLOAT_EQ(e->Position().x, 0.f);
}

TEST_F(CellAppHandlersTest, EnableDisableWitnessTogglesOnEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::EnableWitness e;
  e.entity_id = 100;
  app_.OnEnableWitness({}, nullptr, e);
  EXPECT_TRUE(app_.FindRealEntity(100)->HasWitness());

  cellapp::DisableWitness d{100};
  app_.OnDisableWitness({}, nullptr, d);
  EXPECT_FALSE(app_.FindRealEntity(100)->HasWitness());
}

TEST_F(CellAppHandlersTest, CreateCellEntityDoesNotAutoEnableWitness) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  auto* entity = app_.FindRealEntity(100);
  ASSERT_NE(entity, nullptr);
  EXPECT_FALSE(entity->HasWitness());
}

TEST_F(CellAppHandlersTest, OnSetAoIRadiusUpdatesActiveWitness) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  cellapp::EnableWitness e{100};
  app_.OnEnableWitness({}, nullptr, e);
  auto* entity = app_.FindRealEntity(100);
  ASSERT_TRUE(entity->HasWitness());

  cellapp::SetAoIRadius s;
  s.entity_id = 100;
  s.radius = 42.5f;
  s.hysteresis = 7.f;
  app_.OnSetAoIRadius({}, nullptr, s);

  EXPECT_FLOAT_EQ(entity->GetWitness()->AoIRadius(), 42.5f);
  EXPECT_FLOAT_EQ(entity->GetWitness()->Hysteresis(), 7.f);
}

TEST_F(CellAppHandlersTest, OnSetAoIRadiusMissingWitnessIsNoop) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  cellapp::SetAoIRadius s;
  s.entity_id = 100;
  s.radius = 42.5f;
  s.hysteresis = 7.f;
  app_.OnSetAoIRadius({}, nullptr, s);

  EXPECT_FALSE(app_.FindRealEntity(100)->HasWitness());
}

TEST_F(CellAppHandlersTest, NumRealEntitiesExcludesGhosts) {
  EXPECT_EQ(app_.NumRealEntities(), 0u);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(101, 1));
  EXPECT_EQ(app_.NumRealEntities(), 2u);

  auto* ent = app_.FindRealEntity(100);
  ASSERT_NE(ent, nullptr);
  ent->ConvertRealToGhost(/*new_real_channel=*/nullptr);
  EXPECT_EQ(app_.NumRealEntities(), 1u);
}

TEST_F(CellAppHandlersTest, PersistentLoadStartsAtZero) {
  EXPECT_FLOAT_EQ(app_.PersistentLoad(), 0.f);
}

TEST_F(CellAppHandlersTest, OnSetAoIRadiusClampsToMax) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  cellapp::EnableWitness e{100};
  app_.OnEnableWitness({}, nullptr, e);

  cellapp::SetAoIRadius s;
  s.entity_id = 100;
  s.radius = 10'000.f;
  s.hysteresis = 5.f;
  app_.OnSetAoIRadius({}, nullptr, s);

  EXPECT_FLOAT_EQ(app_.FindRealEntity(100)->GetWitness()->AoIRadius(), 500.f);
}

namespace {

auto PackRpcId(uint8_t direction, uint16_t type_index, uint8_t method_index) -> uint32_t {
  return (static_cast<uint32_t>(direction) << 22) | (static_cast<uint32_t>(type_index) << 8) |
         static_cast<uint32_t>(method_index);
}

auto RegisterTypeWithRpc(uint16_t type_id, uint32_t rpc_id, ExposedScope scope) {
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

TEST_F(CellAppHandlersTest, ClientCellRpcRejectsUntrustedSource) {
  const uint32_t kCellRpc = PackRpcId(0x02, 1, 1);
  RegisterTypeWithRpc(1, kCellRpc, ExposedScope::kAllClients);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(500, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 500;
  msg.source_entity_id = 500;
  msg.rpc_id = kCellRpc;

  const Address untrusted(0x7F000001u, 12345);
  app_.OnClientCellRpcForward(untrusted, nullptr, msg);
  SUCCEED();

  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, ClientCellRpcRejectsUnknownRpcId) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = 100;
  msg.source_entity_id = 100;
  msg.rpc_id = 0xDEADBEEF;
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
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, ClientCellRpcRejectsWrongDirection) {
  const uint16_t kTypeId = 1;
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
  msg.source_entity_id = 200;
  msg.rpc_id = kCellRpc;
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
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, InternalCellRpcBypassesExposedCheck) {
  const uint16_t kTypeId = 1;
  const uint32_t kCellRpc = PackRpcId(0x02, kTypeId, 1);
  RegisterTypeWithRpc(kTypeId, kCellRpc, ExposedScope::kNone);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));

  cellapp::InternalCellRpc msg;
  msg.target_entity_id = 100;
  msg.rpc_id = kCellRpc;
  app_.OnInternalCellRpc({}, nullptr, msg);
}

TEST_F(CellAppHandlersTest, CreateGhostWithNullChannelRejected) {
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  cellapp::CreateGhost msg;
  msg.entity_id = 500;
  msg.type_id = 1;
  msg.space_id = 1;
  msg.position = {0, 0, 0};
  msg.direction = {1, 0, 0};
  msg.on_ground = false;
  msg.real_cellapp_addr = Address(0x7F000001u, 30001);
  msg.base_addr = Address(0x7F000001u, 20000);
  msg.event_seq = 0;
  msg.volatile_seq = 0;

  app_.OnCreateGhost({}, /*ch=*/nullptr, msg);

  EXPECT_EQ(app_.FindEntity(500), nullptr);
}

TEST_F(CellAppHandlersTest, GhostPositionUpdateRejectsNaN) {
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  cellapp::CreateGhost cg;
  cg.entity_id = 600;
  cg.type_id = 1;
  cg.space_id = 1;
  cg.position = {10, 0, 20};
  cg.direction = {1, 0, 0};
  cg.on_ground = false;
  cg.real_cellapp_addr = Address(0x7F000001u, 30001);
  cg.base_addr = Address(0x7F000001u, 20000);
  cg.event_seq = 0;
  cg.volatile_seq = 0;

  app_.OnCreateGhost({}, FakeChannel(0xBEEF), cg);

  auto* ghost = app_.FindEntity(600);
  ASSERT_NE(ghost, nullptr);
  EXPECT_FLOAT_EQ(ghost->Position().x, 10.f);
  EXPECT_FLOAT_EQ(ghost->Position().z, 20.f);

  cellapp::GhostPositionUpdate upd;
  upd.entity_id = 600;
  upd.position = {std::nanf(""), 0, 0};
  upd.direction = {1, 0, 0};
  upd.on_ground = true;
  upd.volatile_seq = 1;
  app_.OnGhostPositionUpdate({}, nullptr, upd);

  EXPECT_FLOAT_EQ(ghost->Position().x, 10.f);
  EXPECT_FLOAT_EQ(ghost->Position().z, 20.f);
}

// Dead Channel* entries must be scrubbed before GhostMaintainer can
// reuse a pointer that NetworkInterface has condemned.
TEST_F(CellAppHandlersTest, PeerDeathDropsOrphanGhostsAndClearsHaunts) {
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  auto* dying_ch = FakeChannel(0xDEAD);
  auto* other_ch = FakeChannel(0xCAFE);

  cellapp::CreateGhost cg;
  cg.entity_id = 700;
  cg.type_id = 1;
  cg.space_id = 1;
  cg.position = {0, 0, 0};
  cg.direction = {1, 0, 0};
  cg.real_cellapp_addr = Address(0x7F000001u, 40001);
  app_.OnCreateGhost({}, dying_ch, cg);
  ASSERT_NE(app_.FindEntity(700), nullptr);

  cellapp::CreateGhost cg_ok;
  cg_ok.entity_id = 701;
  cg_ok.type_id = 1;
  cg_ok.space_id = 1;
  cg_ok.position = {5, 0, 5};
  cg_ok.direction = {1, 0, 0};
  cg_ok.real_cellapp_addr = Address(0x7F000001u, 40002);
  app_.OnCreateGhost({}, other_ch, cg_ok);
  ASSERT_NE(app_.FindEntity(701), nullptr);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(800, 1, {20, 0, 20}));
  auto* real = app_.FindRealEntity(800);
  ASSERT_NE(real, nullptr);
  ASSERT_TRUE(real->IsReal());
  auto* rd = real->GetRealData();
  ASSERT_NE(rd, nullptr);
  const Address dying_addr(0x7F000001u, 40001);
  const Address other_addr(0x7F000001u, 40002);
  ASSERT_TRUE(rd->AddHaunt(dying_ch, dying_addr));
  ASSERT_TRUE(rd->AddHaunt(other_ch, other_addr));
  ASSERT_EQ(rd->HauntCount(), 2u);

  app_.OnPeerCellAppDeath(Address(0x7F000001u, 40001), dying_ch);

  EXPECT_EQ(app_.FindEntity(700), nullptr) << "orphan Ghost should be dropped";
  EXPECT_NE(app_.FindEntity(701), nullptr) << "unrelated Ghost must survive";
  EXPECT_EQ(rd->HauntCount(), 1u) << "dying peer's Haunt should be gone";
  EXPECT_TRUE(rd->HasHaunt(other_ch)) << "surviving peer's Haunt untouched";
}

// OnClientRpcBroadcast finds the Ghost mirror and fans out via local
// Observers; non-Ghost / unknown entity logs and drops.
TEST_F(CellAppHandlersTest, OnClientRpcBroadcast_RejectsUnknownAndNonGhost) {
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(900, 1, {0, 0, 0}));
  ASSERT_NE(app_.FindRealEntity(900), nullptr);

  cellapp::ClientRpcBroadcast unknown;
  unknown.source_entity_id = 99999;
  unknown.rpc_id = 0x000401;
  unknown.target = 2;
  app_.OnClientRpcBroadcast({}, nullptr, unknown);

  cellapp::ClientRpcBroadcast on_real;
  on_real.source_entity_id = 900;
  on_real.rpc_id = 0x000401;
  on_real.target = 2;
  app_.OnClientRpcBroadcast({}, nullptr, on_real);

  EXPECT_NE(app_.FindRealEntity(900), nullptr);
}

// Channel pointer may already be freed when HandlePeerLost runs, so the
// sweep is address-keyed; the second death signal is a no-op.
TEST_F(CellAppHandlersTest, HandlePeerLost_AddressKeyed_AndIdempotent) {
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  auto* dying_ch = FakeChannel(0xDEAD);
  auto* other_ch = FakeChannel(0xCAFE);
  const Address dying_addr(0x7F000001u, 40001);
  const Address other_addr(0x7F000001u, 40002);

  cellapp::CreateGhost cg;
  cg.entity_id = 700;
  cg.type_id = 1;
  cg.space_id = 1;
  cg.position = {0, 0, 0};
  cg.direction = {1, 0, 0};
  cg.real_cellapp_addr = dying_addr;
  app_.OnCreateGhost({}, dying_ch, cg);

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(800, 1, {20, 0, 20}));
  auto* real = app_.FindRealEntity(800);
  ASSERT_NE(real, nullptr);
  auto* rd = real->GetRealData();
  ASSERT_TRUE(rd->AddHaunt(dying_ch, dying_addr));
  ASSERT_TRUE(rd->AddHaunt(other_ch, other_addr));
  ASSERT_EQ(rd->HauntCount(), 2u);

  app_.HandlePeerLost(dying_addr);

  EXPECT_EQ(app_.FindEntity(700), nullptr);
  EXPECT_EQ(rd->HauntCount(), 1u);
  EXPECT_TRUE(rd->HasHaunt(other_ch));

  app_.HandlePeerLost(dying_addr);
  EXPECT_EQ(rd->HauntCount(), 1u);
  EXPECT_TRUE(rd->HasHaunt(other_ch));
}

namespace {

auto MakeOwnerSpace(CellApp& app, SpaceID space_id, cellappmgr::CellID primary_cell_id,
                    uint16_t self_port) -> Space* {
  cellapp::CreateSpace cs;
  cs.space_id = space_id;
  app.OnCreateSpace({}, nullptr, cs);
  auto* space = app.FindSpace(space_id);
  space->AddLocalCell(std::make_unique<Cell>(*space, primary_cell_id, CellBounds{}));
  CellInfo info;
  info.cell_id = primary_cell_id;
  info.cellapp_addr = Address(0x7F000001u, self_port);
  BSPTree tree;
  tree.InitSingleCell(info);
  space->SetBspTree(std::move(tree));
  return space;
}

auto MakeGhostSpace(CellApp& app, SpaceID space_id, cellappmgr::CellID primary_cell_id,
                    cellappmgr::CellID local_cell_id, uint16_t owner_port,
                    uint16_t self_port) -> Space* {
  cellapp::CreateSpace cs;
  cs.space_id = space_id;
  app.OnCreateSpace({}, nullptr, cs);
  auto* space = app.FindSpace(space_id);
  space->AddLocalCell(std::make_unique<Cell>(*space, local_cell_id, CellBounds{}));
  CellInfo primary;
  primary.cell_id = primary_cell_id;
  primary.cellapp_addr = Address(0x7F000001u, owner_port);
  BSPTree tree;
  tree.InitSingleCell(primary);
  CellInfo right;
  right.cell_id = local_cell_id;
  right.cellapp_addr = Address(0x7F000001u, self_port);
  (void)tree.Split(primary_cell_id, BSPAxis::kX, 0.f, right);
  space->SetBspTree(std::move(tree));
  return space;
}

}  // namespace

TEST_F(CellAppHandlersTest, OnSpaceDataUpdate_AppliesValueLocally) {
  auto* space = MakeGhostSpace(app_, /*space_id=*/7, /*primary=*/1, /*local=*/2,
                               /*owner_port=*/30001, /*self_port=*/30002);
  cellapp::SpaceDataUpdate msg;
  msg.space_id = 7;
  msg.key_id = 42;
  msg.value = {0xAA, 0xBB};
  app_.OnSpaceDataUpdate(Address(0x7F000001u, 30001), nullptr, msg);

  const auto* v = space->Data().Get(42);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(*v, (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST_F(CellAppHandlersTest, OnSpaceDataDelete_RemovesKeyLocally) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002);
  space->Data().Set(42, std::vector<uint8_t>{0xAA});

  cellapp::SpaceDataDelete msg;
  msg.space_id = 7;
  msg.key_id = 42;
  app_.OnSpaceDataDelete(Address(0x7F000001u, 30001), nullptr, msg);

  EXPECT_FALSE(space->Data().Contains(42));
}

TEST_F(CellAppHandlersTest, OnSpaceDataSnapshot_OverwritesAndMarksInitialized) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002);
  space->Data().Set(99, std::vector<uint8_t>{0xFF});  // stale local entry
  EXPECT_FALSE(space->IsDataInitialized());

  cellapp::SpaceDataSnapshot snap;
  snap.space_id = 7;
  snap.entries.push_back({1, {0x01}});
  snap.entries.push_back({2, {0x02, 0x03}});
  app_.OnSpaceDataSnapshot(Address(0x7F000001u, 30001), nullptr, snap);

  EXPECT_TRUE(space->IsDataInitialized());
  EXPECT_FALSE(space->Data().Contains(99));
  ASSERT_NE(space->Data().Get(1), nullptr);
  EXPECT_EQ(*space->Data().Get(1), (std::vector<uint8_t>{0x01}));
}

TEST_F(CellAppHandlersTest, OnSpaceDataSnapshotRequest_NonOwnerIsNoop) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002);
  ASSERT_FALSE(space->IsOwner());

  cellapp::SpaceDataSnapshotRequest req;
  req.space_id = 7;
  // Non-owner should silently drop — no reply path attempted (would crash
  // on the fake peer channel below otherwise).
  app_.OnSpaceDataSnapshotRequest(Address(0x7F000001u, 30001), nullptr, req);
}

TEST_F(CellAppHandlersTest, SetSpaceData_OwnerWritesLocally) {
  auto* space = MakeOwnerSpace(app_, 7, 1, 30001);
  ASSERT_TRUE(space->IsOwner());

  const uint8_t value[] = {0xDE, 0xAD};
  app_.SetSpaceData(7, 42, std::span<const uint8_t>(value, 2));

  ASSERT_NE(space->Data().Get(42), nullptr);
  EXPECT_EQ(*space->Data().Get(42), (std::vector<uint8_t>{0xDE, 0xAD}));
}

TEST_F(CellAppHandlersTest, RemoveSpaceData_OwnerRemovesLocally) {
  auto* space = MakeOwnerSpace(app_, 7, 1, 30001);
  space->Data().Set(42, std::vector<uint8_t>{0xAA});

  app_.RemoveSpaceData(7, 42);
  EXPECT_FALSE(space->Data().Contains(42));
}

TEST_F(CellAppHandlersTest, OwnerMarksDataInitializedOnSetBspTree) {
  auto* space = MakeOwnerSpace(app_, 7, 1, 30001);
  EXPECT_TRUE(space->IsDataInitialized());
}

// Ghost-lifecycle wire-up tests: verify cellapp.cc invokes restore_ghost_fn /
// destroy_ghost_fn at the five transition points, with correct ordering vs
// the Real-side restore_entity_fn / entity_migrating_out_fn.

TEST_F(CellAppHandlersTest, OnCreateGhostFiresRestoreGhostCallback) {
  EnableGhostLifecycleCallbacks();
  auto msg = MakeGhost(555, {1, 0, 1});
  msg.type_id = 3;
  msg.other_snapshot = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), msg);

  ASSERT_EQ(ghost_calls_.size(), 1u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kRestoreGhost);
  EXPECT_EQ(ghost_calls_[0].entity_id, 555u);
  EXPECT_EQ(ghost_calls_[0].type_id, 3u);
  EXPECT_EQ(ghost_calls_[0].snapshot_len, 3);
}

TEST_F(CellAppHandlersTest, OnDeleteGhostFiresDestroyGhostCallback) {
  EnableGhostLifecycleCallbacks();
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(600));
  ghost_calls_.clear();

  cellapp::DeleteGhost dg{600};
  app_.OnDeleteGhost({}, FakeChannel(0xBEEF), dg);

  ASSERT_EQ(ghost_calls_.size(), 1u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[0].entity_id, 600u);
}

TEST_F(CellAppHandlersTest, OffloadOnExistingGhostFiresDestroyGhostThenRestoreEntity) {
  EnableGhostLifecycleCallbacks();
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(700));
  ghost_calls_.clear();

  cellapp::OffloadEntity msg;
  msg.entity_id = 700;
  msg.type_id = 1;
  msg.space_id = 1;
  msg.position = {2, 0, 2};
  msg.direction = {1, 0, 0};
  // restore_entity_fn only fires when persistent_blob is non-empty.
  msg.persistent_blob = {std::byte{0x11}};
  app_.OnOffloadEntity({}, /*ch=*/nullptr, msg);

  // destroy_ghost MUST run before restore_entity, else RestoreEntity sees an
  // existing C# Ghost instance and skips OnInit on the promoted Real.
  ASSERT_GE(ghost_calls_.size(), 2u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[0].entity_id, 700u);
  EXPECT_EQ(ghost_calls_[1].kind, GhostCall::kRestoreEntity);
  EXPECT_EQ(ghost_calls_[1].entity_id, 700u);
}

TEST_F(CellAppHandlersTest, RevertPendingOffloadFiresDestroyGhostThenRestoreEntity) {
  EnableGhostLifecycleCallbacks();
  // Stage a Ghost as if ProcessOffload had already converted Real->Ghost and
  // recreated the C# Ghost via restore_ghost_fn.
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(900));

  CellApp::PendingOffload po;
  po.target_addr = Address{0x7F000001u, 26002};
  po.sent_at = std::chrono::steady_clock::now();
  po.space_id = 1;
  po.type_id = 1;
  // restore_entity_fn fires only when persistent_blob is non-empty.
  po.persistent_blob = {std::byte{0x55}};
  app_.PendingOffloadsForTest()[900] = std::move(po);
  ghost_calls_.clear();

  app_.RevertPendingOffload(900, "test");

  ASSERT_GE(ghost_calls_.size(), 2u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[0].entity_id, 900u);
  EXPECT_EQ(ghost_calls_[1].kind, GhostCall::kRestoreEntity);
  EXPECT_EQ(ghost_calls_[1].entity_id, 900u);
}

TEST_F(CellAppHandlersTest, PeerCellAppDeathFiresDestroyGhostForOrphans) {
  EnableGhostLifecycleCallbacks();
  Channel* dying = FakeChannel(0xDEAD);
  app_.OnCreateGhost({}, dying, MakeGhost(800));
  ghost_calls_.clear();

  app_.OnPeerCellAppDeath(Address{0x7F000001u, 26002}, dying);

  ASSERT_EQ(ghost_calls_.size(), 1u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[0].entity_id, 800u);
}

}  // namespace
}  // namespace atlas
