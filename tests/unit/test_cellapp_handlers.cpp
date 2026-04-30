#include <cmath>
#include <cstdint>
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
#include "real_entity_data.h"
#include "space.h"

namespace atlas {
namespace {

class CellAppHandlersTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test_cellapp_handlers"};
  NetworkInterface network_{dispatcher_};
  CellApp app_{dispatcher_, network_};

  void SetUp() override {
    EntityDefRegistry::Instance().clear();
    app_.InsertTrustedBaseAppForTest(Address{});
  }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }

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

TEST_F(CellAppHandlersTest, DestroyCellEntityRemovesEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(100, 1));
  ASSERT_NE(app_.FindRealEntity(100), nullptr);

  cellapp::DestroyCellEntity d{100};
  app_.OnDestroyCellEntity({}, nullptr, d);

  EXPECT_EQ(app_.FindRealEntity(100), nullptr);
  EXPECT_EQ(app_.FindSpace(1)->EntityCount(), 0u);
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

}  // namespace
}  // namespace atlas
