#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "clrscript/native_api_provider.h"
#include "entitydef/entity_def_registry.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "test_null_channel.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "real_entity_data.h"
#include "space.h"

namespace atlas {
namespace {

class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
      : Channel(dispatcher, table, remote) {}

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    if (fail_next_send_) {
      fail_next_send_ = false;
      return Error{ErrorCode::kInternalError, "injected send failure"};
    }
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

  void FailNextSend() { fail_next_send_ = true; }

  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 private:
  bool fail_next_send_{false};
  std::vector<std::vector<std::byte>> sends_;
};

auto SpaceDataSnapshotRequests(const RecordingChannel& ch)
    -> std::vector<cellapp::SpaceDataSnapshotRequest> {
  std::vector<cellapp::SpaceDataSnapshotRequest> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellapp::SpaceDataSnapshotRequest::Descriptor().id) continue;
    auto msg = cellapp::SpaceDataSnapshotRequest::Deserialize(reader);
    if (msg.HasValue()) out.push_back(*msg);
  }
  return out;
}

auto AddCellToSpaceAcks(const RecordingChannel& ch)
    -> std::vector<cellappmgr::AddCellToSpaceAck> {
  std::vector<cellappmgr::AddCellToSpaceAck> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::AddCellToSpaceAck::Descriptor().id) continue;
    auto msg = cellappmgr::AddCellToSpaceAck::Deserialize(reader);
    if (msg.HasValue()) out.push_back(*msg);
  }
  return out;
}

auto InformCellLoads(const RecordingChannel& ch) -> std::vector<cellappmgr::InformCellLoad> {
  std::vector<cellappmgr::InformCellLoad> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::InformCellLoad::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = cellappmgr::InformCellLoad::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

auto CellEntityCreateFailures(const RecordingChannel& ch)
    -> std::vector<baseapp::CellEntityCreateFailed> {
  std::vector<baseapp::CellEntityCreateFailed> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != baseapp::CellEntityCreateFailed::Descriptor().id) continue;
    auto msg = baseapp::CellEntityCreateFailed::Deserialize(reader);
    if (msg.HasValue()) out.push_back(*msg);
  }
  return out;
}

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
std::vector<std::byte>* g_serialize_blob = nullptr;

extern "C" void GhostTestRestoreGhost(uint32_t eid, uint16_t tid, const uint8_t*, int32_t len) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kRestoreGhost, eid, tid, len});
}
extern "C" void GhostTestDestroyGhost(uint32_t eid) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kDestroyGhost, eid, 0, 0});
}
extern "C" void GhostTestRestoreEntity(uint32_t eid, uint16_t tid, int64_t,
                                       const uint8_t*, int32_t len) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kRestoreEntity, eid, tid, len});
}
extern "C" void GhostTestEntityDestroyed(uint32_t eid) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kEntityDestroyed, eid, 0, 0});
}
extern "C" void GhostTestMigratingOut(uint32_t eid) {
  if (g_ghost_calls) g_ghost_calls->push_back({GhostCall::kMigratingOut, eid, 0, 0});
}
extern "C" int32_t GhostTestSerializeEntity(uint32_t, uint8_t* out_buf, int32_t out_buf_cap,
                                            int32_t* out_len) {
  if (g_serialize_blob == nullptr) return -1;
  const int32_t len = static_cast<int32_t>(g_serialize_blob->size());
  if (out_len != nullptr) *out_len = len;
  if (out_buf == nullptr || out_buf_cap < len) return len;
  if (len > 0) std::memcpy(out_buf, g_serialize_blob->data(), static_cast<std::size_t>(len));
  return 0;
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

class WatcherCellApp final : public CellApp {
 public:
  using CellApp::CellApp;
  void RegisterWatchersForTest() { RegisterWatchers(); }
  [[nodiscard]] auto MovementServerTickForTest() const -> uint32_t {
    return static_cast<const CellMovementHost*>(this)->MovementServerTick();
  }
};

class CellAppHandlersTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test_cellapp_handlers"};
  NetworkInterface network_{dispatcher_};
  WatcherCellApp app_{dispatcher_, network_};
  // Keeps native_provider_ alive for the test duration; CreateNativeProvider
  // hands back ownership to the caller in production (ScriptApp owns it).
  std::unique_ptr<INativeApiProvider> native_provider_holder_;
  std::vector<GhostCall> ghost_calls_;

  void SetUp() override {
    EntityDefRegistry::Instance().clear();
    app_.InsertTrustedBaseAppForTest(Address{});
    app_.PeerRegistryForTest().InsertForTest(Address{}, test_support::FakeChannel(0xFEED));
    g_ghost_calls = &ghost_calls_;
  }
  void TearDown() override {
    g_ghost_calls = nullptr;
    g_serialize_blob = nullptr;
    EntityDefRegistry::Instance().clear();
  }

  void EnableGhostLifecycleCallbacks(bool with_serialize = false) {
    native_provider_holder_ = app_.CreateNativeProviderForTest();
    GhostTestCallbackTable table{};
    table.restore_entity = reinterpret_cast<void*>(&GhostTestRestoreEntity);
    table.entity_destroyed = reinterpret_cast<void*>(&GhostTestEntityDestroyed);
    table.entity_migrating_out = reinterpret_cast<void*>(&GhostTestMigratingOut);
    if (with_serialize) table.serialize_entity = reinterpret_cast<void*>(&GhostTestSerializeEntity);
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

  auto MakeNativeMovementCommand(uint32_t command_id, uint8_t priority = 0)
      -> NativeMovementCommand {
    NativeMovementCommand command;
    command.command_id = command_id;
    command.type = static_cast<uint8_t>(movement::MovementCommandType::kDash);
    command.start_x = 0.0f;
    command.start_y = 0.0f;
    command.start_z = 0.0f;
    command.target_x = 1.0f;
    command.target_y = 0.0f;
    command.target_z = 0.0f;
    command.duration_ms = 1000;
    command.curve_id = 0;
    command.input_policy =
        static_cast<uint8_t>(movement::MovementCommandInputPolicy::kSuppress);
    command.collision_policy =
        static_cast<uint8_t>(movement::MovementCommandCollisionPolicy::kStop);
    command.priority = priority;
    return command;
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

using test_support::FakeChannel;

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
  ghost.persistent_blob = {std::byte{0xAA}};
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

TEST_F(CellAppHandlersTest, CreateCellEntityPromotesExistingGhostForRestore) {
  EnableGhostLifecycleCallbacks();
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);
  auto* space = app_.FindSpace(1);
  ASSERT_NE(space, nullptr);
  auto* cell = space->AddLocalCell(std::make_unique<Cell>(*space, 1, CellBounds{}));
  ASSERT_NE(cell, nullptr);

  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(201, {2, 0, 2}));
  ghost_calls_.clear();

  auto msg = MakeCreate(201, 1, {9, 0, 9});
  msg.request_id = 201;
  msg.script_init_data = {std::byte{0x11}};
  app_.OnCreateCellEntity({}, nullptr, msg);

  auto* promoted = app_.FindRealEntity(201);
  ASSERT_NE(promoted, nullptr);
  EXPECT_TRUE(cell->HasRealEntity(promoted));
  EXPECT_FLOAT_EQ(promoted->Position().x, 2.f);
  EXPECT_FLOAT_EQ(promoted->Position().z, 2.f);
  ASSERT_GE(ghost_calls_.size(), 2u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[1].kind, GhostCall::kRestoreEntity);
}

TEST_F(CellAppHandlersTest, CreateCellEntityPromotesGhostWithPersistentBlobFallback) {
  EnableGhostLifecycleCallbacks();
  cellapp::CreateSpace cs;
  cs.space_id = 1;
  app_.OnCreateSpace({}, nullptr, cs);

  auto ghost = MakeGhost(202, {2, 0, 2});
  ghost.persistent_blob = {std::byte{0x22}, std::byte{0x33}};
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), ghost);
  ghost_calls_.clear();

  auto msg = MakeCreate(202, 1, {9, 0, 9});
  msg.request_id = 202;
  msg.require_existing_ghost = true;
  app_.OnCreateCellEntity({}, nullptr, msg);

  auto* promoted = app_.FindRealEntity(202);
  ASSERT_NE(promoted, nullptr);
  ASSERT_GE(ghost_calls_.size(), 2u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[1].kind, GhostCall::kRestoreEntity);
  EXPECT_EQ(ghost_calls_[1].snapshot_len, 2);
}

TEST_F(CellAppHandlersTest, GhostSnapshotRefreshUpdatesPersistentBlobFallback) {
  EnableGhostLifecycleCallbacks();

  auto ghost = MakeGhost(204, {2, 0, 2});
  ghost.persistent_blob = {std::byte{0x11}};
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), ghost);

  cellapp::GhostSnapshotRefresh refresh;
  refresh.entity_id = 204;
  refresh.event_seq = 1;
  refresh.persistent_blob = {std::byte{0x44}, std::byte{0x55}, std::byte{0x66}};
  app_.OnGhostSnapshotRefresh({}, nullptr, refresh);
  ghost_calls_.clear();

  auto msg = MakeCreate(204, 1, {9, 0, 9});
  msg.request_id = 204;
  msg.require_existing_ghost = true;
  app_.OnCreateCellEntity({}, nullptr, msg);

  ASSERT_GE(ghost_calls_.size(), 2u);
  EXPECT_EQ(ghost_calls_[1].kind, GhostCall::kRestoreEntity);
  EXPECT_EQ(ghost_calls_[1].snapshot_len, 3);
}

TEST_F(CellAppHandlersTest, CreateCellEntityGhostOnlyRestoreRejectsMissingGhost) {
  EnableGhostLifecycleCallbacks();
  InterfaceTable table;
  RecordingChannel base_ch(dispatcher_, table, Address(0x7F000001u, 20001));

  auto msg = MakeCreate(203, 1, {9, 0, 9});
  msg.request_id = 203;
  msg.require_existing_ghost = true;
  app_.OnCreateCellEntity({}, &base_ch, msg);

  EXPECT_EQ(app_.FindEntity(203), nullptr);
  EXPECT_TRUE(ghost_calls_.empty());
  const auto failures = CellEntityCreateFailures(base_ch);
  ASSERT_EQ(failures.size(), 1u);
  EXPECT_EQ(failures[0].reason,
            baseapp::CellEntityCreateFailureReason::kGhostRequiredMissing);
}

TEST_F(CellAppHandlersTest, CreateCellEntityGhostOnlyRestoreRejectsGhostWithoutBlob) {
  EnableGhostLifecycleCallbacks();
  InterfaceTable table;
  RecordingChannel base_ch(dispatcher_, table, Address(0x7F000001u, 20001));
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(205, {2, 0, 2}));
  ghost_calls_.clear();

  auto msg = MakeCreate(205, 1, {9, 0, 9});
  msg.request_id = 205;
  msg.require_existing_ghost = true;
  app_.OnCreateCellEntity({}, &base_ch, msg);

  auto* entity = app_.FindEntity(205);
  ASSERT_NE(entity, nullptr);
  EXPECT_TRUE(entity->IsGhost());
  EXPECT_TRUE(ghost_calls_.empty());
  const auto failures = CellEntityCreateFailures(base_ch);
  ASSERT_EQ(failures.size(), 1u);
  EXPECT_EQ(failures[0].reason, baseapp::CellEntityCreateFailureReason::kGhostBackupMissing);
}

TEST(CellAppWatchers, GhostPromoteCounterTracksCreateCellEntityRestore) {
  EventDispatcher dispatcher{"test_cellapp_ghost_promote_watcher"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.RegisterWatchersForTest();

  const Address peer_addr(0x7F000001u, 26002);
  app.PeerRegistryForTest().InsertForTest(peer_addr, FakeChannel(0xA11CE));

  cellapp::CreateGhost ghost;
  ghost.entity_id = 301;
  ghost.type_id = 1;
  ghost.space_id = 1;
  ghost.position = {2, 0, 2};
  ghost.direction = {1, 0, 0};
  ghost.real_cellapp_addr = peer_addr;
  app.OnCreateGhost(peer_addr, FakeChannel(0xA11CE), ghost);

  cellapp::CreateCellEntity create;
  create.entity_id = 301;
  create.type_id = 1;
  create.space_id = 1;
  create.position = {9, 0, 9};
  create.direction = {1, 0, 0};
  create.request_id = 301;
  app.OnCreateCellEntity({}, nullptr, create);

  EXPECT_NE(app.FindRealEntity(301), nullptr);
  EXPECT_EQ(app.GetWatcherRegistry().Get("cellapp/ghost_promoted_to_real_total").value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry().Get("cellapp/create_cell_entity_total").value_or(""), "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_restore_empty_total")
                .value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_restore_payload_total")
                .value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_restore_ghost_backup_total")
                .value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("cellapp/create_cell_entity_failures_total").value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_total")
                .value_or(""),
            "0");
}

TEST(CellAppWatchers, CreateCellEntityRestoreSourceCountersTrackPayloadAndGhostBackup) {
  EventDispatcher dispatcher{"test_cellapp_restore_source_watcher"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.RegisterWatchersForTest();

  cellapp::CreateCellEntity payload_create;
  payload_create.entity_id = 401;
  payload_create.type_id = 1;
  payload_create.space_id = 1;
  payload_create.position = {1, 0, 1};
  payload_create.direction = {1, 0, 0};
  payload_create.request_id = 401;
  payload_create.script_init_data = {std::byte{0x44}};
  payload_create.cellapp_death_restore = true;
  app.OnCreateCellEntity({}, nullptr, payload_create);

  const Address peer_addr(0x7F000001u, 26002);
  app.PeerRegistryForTest().InsertForTest(peer_addr, FakeChannel(0xA11CE));
  cellapp::CreateGhost ghost;
  ghost.entity_id = 402;
  ghost.type_id = 1;
  ghost.space_id = 1;
  ghost.position = {2, 0, 2};
  ghost.direction = {1, 0, 0};
  ghost.real_cellapp_addr = peer_addr;
  ghost.persistent_blob = {std::byte{0x55}};
  app.OnCreateGhost(peer_addr, FakeChannel(0xA11CE), ghost);

  cellapp::CreateCellEntity ghost_restore;
  ghost_restore.entity_id = 402;
  ghost_restore.type_id = 1;
  ghost_restore.space_id = 1;
  ghost_restore.position = {9, 0, 9};
  ghost_restore.direction = {1, 0, 0};
  ghost_restore.request_id = 402;
  ghost_restore.require_existing_ghost = true;
  ghost_restore.cellapp_death_restore = true;
  app.OnCreateCellEntity({}, nullptr, ghost_restore);

  EXPECT_EQ(app.GetWatcherRegistry().Get("cellapp/create_cell_entity_total").value_or(""), "2");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_restore_payload_total")
                .value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_restore_ghost_backup_total")
                .value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_restore_empty_total")
                .value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("cellapp/create_cell_entity_failures_total").value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_total")
                .value_or(""),
            "2");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_payload_total")
                .value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_ghost_backup_total")
                .value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_empty_total")
                .value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_failures_total")
                .value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_promoted_total")
                .value_or(""),
            "1");
}

TEST(CellAppWatchers, CreateCellEntityDeathRestoreFailureCounterTracksFailures) {
  EventDispatcher dispatcher{"test_cellapp_death_restore_failure_watcher"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.RegisterWatchersForTest();

  cellapp::CreateCellEntity create;
  create.entity_id = 403;
  create.type_id = 1;
  create.space_id = 1;
  create.position = {1, 0, 1};
  create.direction = {1, 0, 0};
  create.request_id = 403;
  create.require_existing_ghost = true;
  create.cellapp_death_restore = true;
  app.OnCreateCellEntity({}, nullptr, create);

  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_total")
                .value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("cellapp/create_cell_entity_death_restore_failures_total")
                .value_or(""),
            "1");
}

TEST(CellAppWatchers, MovementStepTimeWatcherRecordsTick) {
  EventDispatcher dispatcher{"test_cellapp_movement_step_watcher"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.InsertTrustedBaseAppForTest(Address{});
  app.RegisterWatchersForTest();

  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/step_time/count").value_or(""), "0");
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/input_rate_limited_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/input_invalid_dropped_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/step_time_us_p95").has_value());

  cellapp::CreateCellEntity create;
  create.entity_id = 302;
  create.type_id = 1;
  create.space_id = 1;
  create.direction = {0, 0, 1};
  app.OnCreateCellEntity({}, nullptr, create);

  cellapp::ClientMovementInputForward input;
  input.source_entity_id = 302;
  input.target_entity_id = 302;
  input.frames.push_back({1, 1, 0, 127, 0, 0, 0, 16});
  app.OnClientMovementInputForward({}, nullptr, input);
  app.TickMovementSystemForTest(1.0f / 30.0f);

  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/step_time/count").value_or(""), "1");
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/step_time/p95_us").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/step_time_us_p95").has_value());
}

TEST(CellAppWatchers, MovementPositionHistoryRecordsTick) {
  EventDispatcher dispatcher{"test_cellapp_movement_history_watcher"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.InsertTrustedBaseAppForTest(Address{});
  app.RegisterWatchersForTest();

  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/position_history_entities").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/position_history_samples").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry()
                  .Get("movement/position_history_samples_recorded_total")
                  .has_value());

  cellapp::CreateCellEntity create;
  create.entity_id = 304;
  create.type_id = 1;
  create.space_id = 1;
  create.direction = {0, 0, 1};
  app.OnCreateCellEntity({}, nullptr, create);

  cellapp::ClientMovementInputForward input;
  input.source_entity_id = 304;
  input.target_entity_id = 304;
  input.frames.push_back({1, 1, 0, 127, 0, 0, 0, 16});
  app.OnClientMovementInputForward({}, nullptr, input);
  app.TickMovementSystemForTest(1.0f / 30.0f);

  const auto* history = app.MovementSystemForTest().position_history().Find(304);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->back().server_tick, 0u);
  EXPECT_GT(history->back().state.position.z, 0.0f);
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/position_history_entities").value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/position_history_samples").value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry()
                .Get("movement/position_history_samples_recorded_total")
                .value_or(""),
            "1");
}

TEST(CellAppWatchers, MovementCommandWatchersTrackLifecycle) {
  EventDispatcher dispatcher{"test_cellapp_movement_command_watcher"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.RegisterWatchersForTest();

  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/active_commands").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/command_started_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/command_ended_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/command_completed_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/command_cancelled_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/command_collision_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/command_invalid_total").has_value());
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/active_commands").value_or(""), "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_started_total").value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_ended_total").value_or(""), "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_completed_total").value_or(""),
            "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_cancelled_total").value_or(""),
            "0");

  cellapp::CreateCellEntity create;
  create.entity_id = 305;
  create.type_id = 1;
  create.space_id = 1;
  create.direction = {1, 0, 0};
  app.OnCreateCellEntity({}, nullptr, create);

  auto native_provider = app.CreateNativeProviderForTest();
  ASSERT_NE(native_provider, nullptr);
  NativeMovementCommand command;
  command.command_id = 81;
  command.type = static_cast<uint8_t>(movement::MovementCommandType::kDash);
  command.start_x = 0.0f;
  command.start_y = 0.0f;
  command.start_z = 0.0f;
  command.target_x = 1.0f;
  command.target_y = 0.0f;
  command.target_z = 0.0f;
  command.duration_ms = 1000;
  command.curve_id = 0;
  command.input_policy =
      static_cast<uint8_t>(movement::MovementCommandInputPolicy::kSuppress);
  command.collision_policy =
      static_cast<uint8_t>(movement::MovementCommandCollisionPolicy::kStop);

  EXPECT_TRUE(native_provider->SetMovementCommand(305, command));
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/active_commands").value_or(""), "1");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_started_total").value_or(""),
            "1");

  EXPECT_TRUE(native_provider->ClearMovementCommand(305, 81));
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/active_commands").value_or(""), "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_ended_total").value_or(""), "1");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_cancelled_total").value_or(""),
            "1");

  EXPECT_TRUE(native_provider->SetMovementCommand(305, command));
  app.TickMovementSystemForTest(1.0f);

  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/active_commands").value_or(""), "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_started_total").value_or(""),
            "2");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_ended_total").value_or(""), "2");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/command_completed_total").value_or(""),
            "1");
}

TEST(CellAppWatchers, MovementInputDropBreakdownWatchers) {
  EventDispatcher dispatcher{"test_cellapp_movement_input_watchers"};
  NetworkInterface network{dispatcher};
  WatcherCellApp app{dispatcher, network};
  app.InsertTrustedBaseAppForTest(Address{});
  app.RegisterWatchersForTest();

  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/input_stale_dropped_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/input_seq_gap_dropped_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/input_overflow_dropped_total").has_value());
  ASSERT_TRUE(app.GetWatcherRegistry().Get("movement/input_invalid_dropped_total").has_value());

  cellapp::CreateCellEntity create;
  create.entity_id = 303;
  create.type_id = 1;
  create.space_id = 1;
  create.direction = {0, 0, 1};
  app.OnCreateCellEntity({}, nullptr, create);

  cellapp::ClientMovementInputForward input;
  input.source_entity_id = 303;
  input.target_entity_id = 303;
  input.frames.push_back({10, 10, 0, 127, 0, 0, 0, 16});
  app.OnClientMovementInputForward({}, nullptr, input);

  app.OnClientMovementInputForward({}, nullptr, input);
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/input_stale_dropped_total").value_or(""),
            "1");

  input.frames.clear();
  input.frames.push_back({400, 11, 0, 127, 0, 0, 0, 16});
  app.OnClientMovementInputForward({}, nullptr, input);
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/input_seq_gap_dropped_total").value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/input_overflow_dropped_total").value_or(""),
            "0");

  input.frames.clear();
  input.frames.push_back({401, 12, 0, 127, 0, 0, 0, 0});
  app.OnClientMovementInputForward({}, nullptr, input);
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/input_invalid_dropped_total").value_or(""),
            "1");
  EXPECT_EQ(app.GetWatcherRegistry().Get("movement/input_dropped_total").value_or(""),
            "3");
}

TEST_F(CellAppHandlersTest, CreateGhostOnRealIsIdempotentNoOp) {
  // Stale CreateGhost after local promote is a no-op rather than an
  // id-collision error.
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

TEST_F(CellAppHandlersTest, ClientMovementInputQueuesTrustedRealFrames) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(700, 1));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 700;
  msg.target_entity_id = 700;
  msg.frames.push_back({10, 100, 127, 0, 0, 0, 0, 16});
  msg.frames.push_back({11, 101, 0, 127, 0, 0, 0, 16});

  app_.OnClientMovementInputForward({}, nullptr, msg);

  auto drained = app_.MovementSystemForTest().input_buffer().Drain(700, 4);
  ASSERT_EQ(drained.size(), 2u);
  EXPECT_EQ(drained[0].seq, 10u);
  EXPECT_EQ(drained[1].move_z, 127);
}

TEST_F(CellAppHandlersTest, TickMovementSystemConsumesOneInputFrame) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(710, 1));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 710;
  msg.target_entity_id = 710;
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  msg.frames.push_back({11, 101, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(1.0f / 30.0f);

  auto* entity = app_.FindRealEntity(710);
  ASSERT_NE(entity, nullptr);
  EXPECT_GT(entity->Position().z, 0.0f);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(710), 1u);

  const auto* state = app_.MovementSystemForTest().state_store().Find(710);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->last_processed_input_seq, 10u);

  const auto* replication = entity->GetReplicationState();
  ASSERT_NE(replication, nullptr);
  EXPECT_EQ(replication->latest_volatile_seq, 1u);
}

TEST_F(CellAppHandlersTest, TickMovementSystemStepsOnStaticPhysicsBox) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(714, 1));
  auto* physics_query = app_.MovementPhysicsQueryForTest(1);
  ASSERT_NE(physics_query, nullptr);
  physics_query->AddBox(physics::StaticBox{{-0.5f, 0.0f, 0.5f},
                                           {0.5f, 0.25f, 1.2f},
                                           0});
  auto* entity = app_.FindRealEntity(714);
  ASSERT_NE(entity, nullptr);
  entity->SetOnGround(true);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 714;
  msg.target_entity_id = 714;
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.2f);

  EXPECT_NEAR(entity->Position().y, 0.25f, 0.001f);
  EXPECT_GT(entity->Position().z, 0.9f);
  EXPECT_TRUE(entity->OnGround());
}

TEST_F(CellAppHandlersTest, TickMovementSystemGroundProbeUsesCapsuleRadius) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(718, 1, {0.0f, 0.3f, 0.0f}));
  auto* physics_query = app_.MovementPhysicsQueryForTest(1);
  ASSERT_NE(physics_query, nullptr);
  physics_query->AddBox(physics::StaticBox{{0.3f, 0.0f, -0.2f},
                                           {0.6f, 0.25f, 0.2f},
                                           0});
  auto* entity = app_.FindRealEntity(718);
  ASSERT_NE(entity, nullptr);
  entity->SetOnGround(true);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 718;
  msg.target_entity_id = 718;
  msg.frames.push_back({10, 100, 0, 0, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.1f);

  EXPECT_NEAR(entity->Position().y, 0.25f, 0.001f);
  EXPECT_TRUE(entity->OnGround());
}

TEST_F(CellAppHandlersTest, TickMovementSystemStopsLargeFallOnStaticGround) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(719, 1, {0.0f, 0.2f, 0.0f}));
  auto* entity = app_.FindRealEntity(719);
  ASSERT_NE(entity, nullptr);
  entity->SetOnGround(false);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 719;
  msg.target_entity_id = 719;
  msg.frames.push_back({10, 100, 0, 0, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.1f);

  EXPECT_NEAR(entity->Position().y, 0.0f, 0.001f);
  EXPECT_TRUE(entity->OnGround());
}

TEST_F(CellAppHandlersTest, TickMovementSystemKeepsJumpCeilingHitAirborne) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(721, 1));
  auto* physics_query = app_.MovementPhysicsQueryForTest(1);
  ASSERT_NE(physics_query, nullptr);
  physics_query->AddBox(
      physics::StaticBox{{-1.0f, 1.95f, -1.0f}, {1.0f, 2.1f, 1.0f}, 0});
  auto* entity = app_.FindRealEntity(721);
  ASSERT_NE(entity, nullptr);
  entity->SetOnGround(true);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 721;
  msg.target_entity_id = 721;
  msg.frames.push_back({10, 100, 0, 0, 0, 0, movement::kInputButtonJump, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.1f);

  EXPECT_GT(entity->Position().y, 0.1f);
  EXPECT_FALSE(entity->OnGround());
}

TEST_F(CellAppHandlersTest, TickMovementSystemDepenetratesStaticGround) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(720, 1, {0.0f, -0.05f, 0.0f}));
  auto* entity = app_.FindRealEntity(720);
  ASSERT_NE(entity, nullptr);
  entity->SetOnGround(false);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 720;
  msg.target_entity_id = 720;
  msg.frames.push_back({10, 100, 0, 0, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.1f);

  EXPECT_NEAR(entity->Position().y, 0.0f, 0.001f);
  EXPECT_TRUE(entity->OnGround());
}

TEST_F(CellAppHandlersTest, TickMovementSystemUsesEntitySpacePhysicsQuery) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(715, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(716, 2));
  auto* space1_query = app_.MovementPhysicsQueryForTest(1);
  ASSERT_NE(space1_query, nullptr);
  space1_query->AddBox(physics::StaticBox{{-0.5f, 0.0f, 0.5f},
                                          {0.5f, 0.25f, 1.2f},
                                          0});

  auto* stepped = app_.FindRealEntity(715);
  auto* flat = app_.FindRealEntity(716);
  ASSERT_NE(stepped, nullptr);
  ASSERT_NE(flat, nullptr);
  stepped->SetOnGround(true);
  flat->SetOnGround(true);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 715;
  msg.target_entity_id = 715;
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  msg.source_entity_id = 716;
  msg.target_entity_id = 716;
  msg.frames.clear();
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.2f);

  EXPECT_NEAR(stepped->Position().y, 0.25f, 0.001f);
  EXPECT_NEAR(flat->Position().y, 0.0f, 0.001f);
  EXPECT_GT(flat->Position().z, 0.9f);
}

TEST_F(CellAppHandlersTest, TickMovementSystemRejectsSteepStaticPhysicsPlane) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(717, 1));
  auto* physics_query = app_.MovementPhysicsQueryForTest(1);
  ASSERT_NE(physics_query, nullptr);
  physics_query->AddPlane(physics::StaticPlane{{0.0f, 0.0f, 0.0f},
                                               {0.0f, 0.5f, 0.8660254f},
                                               0});
  auto* entity = app_.FindRealEntity(717);
  ASSERT_NE(entity, nullptr);
  entity->SetOnGround(true);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 717;
  msg.target_entity_id = 717;
  msg.frames.push_back({10, 100, 0, 0, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.2f);

  EXPECT_LT(entity->Position().y, 0.0f);
  EXPECT_FALSE(entity->OnGround());
}

TEST_F(CellAppHandlersTest, TickMovementSystemAdvancesActiveStateWithoutNewInput) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(711, 1));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 711;
  msg.target_entity_id = 711;
  msg.frames.push_back({20, 200, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  app_.TickMovementSystemForTest(1.0f / 30.0f);

  auto* entity = app_.FindRealEntity(711);
  ASSERT_NE(entity, nullptr);
  const auto z_after_input = entity->Position().z;

  app_.TickMovementSystemForTest(1.0f / 30.0f);

  EXPECT_FLOAT_EQ(entity->Position().z, z_after_input);
  const auto* state = app_.MovementSystemForTest().state_store().Find(711);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->last_processed_input_seq, 20u);
  const auto* replication = entity->GetReplicationState();
  ASSERT_NE(replication, nullptr);
  EXPECT_EQ(replication->latest_volatile_seq, 2u);
}

TEST_F(CellAppHandlersTest, TickMovementSystemConsumesScriptMovementIntent) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(712, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  for (int tick = 0; tick < 5; ++tick) {
    app_.NativeProvider()->SetMovementIntent(712, 0.0f, 1.0f, 3.0f, 0);
    app_.TickMovementSystemForTest(1.0f / 30.0f);
  }

  auto* entity = app_.FindRealEntity(712);
  ASSERT_NE(entity, nullptr);
  EXPECT_GT(entity->Position().z, 0.0f);
  EXPECT_FLOAT_EQ(entity->Direction().z, 1.0f);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().TotalQueueDepth(), 0u);

  const auto* state = app_.MovementSystemForTest().state_store().Find(712);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->last_processed_input_seq, 0u);
  EXPECT_NEAR(state->velocity.z, 3.0f, 0.03f);

  const auto* replication = entity->GetReplicationState();
  ASSERT_NE(replication, nullptr);
  EXPECT_EQ(replication->latest_volatile_seq, 5u);
}

TEST_F(CellAppHandlersTest, TickMovementSystemAdvancesMovementCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(723, 1));
  movement::MovementCommand command;
  command.command_id = 77;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {1.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 0;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(723, command));

  app_.TickMovementSystemForTest(0.5f);

  auto* entity = app_.FindRealEntity(723);
  ASSERT_NE(entity, nullptr);
  EXPECT_NEAR(entity->Position().x, 0.5f, 0.001f);
  EXPECT_NEAR(entity->Direction().x, 1.0f, 0.001f);

  const auto* stored = app_.MovementSystemForTest().command_store().Find(723);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->elapsed_ms, 500u);

  const auto* state = app_.MovementSystemForTest().state_store().Find(723);
  ASSERT_NE(state, nullptr);
  EXPECT_NEAR(state->position.x, 0.5f, 0.001f);
  EXPECT_NEAR(state->velocity.x, 1.0f, 0.001f);
  EXPECT_TRUE(app_.MovementSystemForTest().position_history().SampleAt(723, 0).has_value());
}

TEST_F(CellAppHandlersTest, TickMovementSystemStopsMovementCommandOnStaticPhysics) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(727, 1));
  auto* physics_query = app_.MovementPhysicsQueryForTest(1);
  ASSERT_NE(physics_query, nullptr);
  physics_query->AddBox(
      physics::StaticBox{{1.0f, 0.0f, -1.0f}, {1.5f, 2.0f, 1.0f}, 0});

  movement::MovementCommand command;
  command.command_id = 80;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {4.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 0;
  command.collision_policy = movement::MovementCommandCollisionPolicy::kStop;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(727, command));

  app_.TickMovementSystemForTest(1.0f);

  auto* entity = app_.FindRealEntity(727);
  ASSERT_NE(entity, nullptr);
  EXPECT_GT(entity->Position().x, 0.5f);
  EXPECT_LT(entity->Position().x, 1.0f);
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(727), nullptr);

  const auto* state = app_.MovementSystemForTest().state_store().Find(727);
  ASSERT_NE(state, nullptr);
  EXPECT_NEAR(state->velocity.Length(), 0.0f, 0.001f);
}

TEST_F(CellAppHandlersTest, TickMovementSystemCompletesMovementCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(724, 1));
  movement::MovementCommand command;
  command.command_id = 78;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {1.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 0;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(724, command));

  app_.TickMovementSystemForTest(0.5f);
  app_.TickMovementSystemForTest(0.5f);

  auto* entity = app_.FindRealEntity(724);
  ASSERT_NE(entity, nullptr);
  EXPECT_NEAR(entity->Position().x, 1.0f, 0.001f);
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(724), nullptr);
}

TEST_F(CellAppHandlersTest, TickMovementSystemDropsUnknownCommandCurve) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(725, 1));
  movement::MovementCommand command;
  command.command_id = 79;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {1.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 99;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(725, command));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 725;
  msg.target_entity_id = 725;
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(1.0f / 30.0f);

  auto* entity = app_.FindRealEntity(725);
  ASSERT_NE(entity, nullptr);
  EXPECT_GT(entity->Position().z, 0.0f);
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(725), nullptr);
  const auto* state = app_.MovementSystemForTest().state_store().Find(725);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->last_processed_input_seq, 10u);
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCommandQueuesCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(726, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  const auto command = MakeNativeMovementCommand(88);

  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(726, command));
  const auto* stored = app_.MovementSystemForTest().command_store().Find(726);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->command_id, 88u);
  EXPECT_EQ(stored->server_tick, app_.GameTime());

  app_.TickMovementSystemForTest(0.5f);

  auto* entity = app_.FindRealEntity(726);
  ASSERT_NE(entity, nullptr);
  EXPECT_NEAR(entity->Position().x, 0.5f, 0.001f);
}

TEST_F(CellAppHandlersTest, SuppressedMovementCommandDropsBufferedClientInput) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(734, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 734;
  msg.target_entity_id = 734;
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  ASSERT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(734), 1u);

  const auto command = MakeNativeMovementCommand(99);
  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(734, command));
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(734), 0u);

  msg.frames.clear();
  msg.frames.push_back({11, 101, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(734), 0u);

  EXPECT_TRUE(app_.NativeProvider()->ClearMovementCommand(734, 99));
  app_.TickMovementSystemForTest(1.0f / 30.0f);

  auto* entity = app_.FindRealEntity(734);
  ASSERT_NE(entity, nullptr);
  EXPECT_NEAR(entity->Position().z, 0.0f, 0.001f);
  const auto* state = app_.MovementSystemForTest().state_store().Find(734);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->last_processed_input_seq, 0u);
}

TEST_F(CellAppHandlersTest, AllowTurnMovementCommandConsumesInputForDirectionOnly) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(735, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  auto command = MakeNativeMovementCommand(100);
  command.input_policy =
      static_cast<uint8_t>(movement::MovementCommandInputPolicy::kAllowTurn);
  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(735, command));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 735;
  msg.target_entity_id = 735;
  msg.frames.push_back({20, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  app_.TickMovementSystemForTest(0.5f);

  auto* entity = app_.FindRealEntity(735);
  ASSERT_NE(entity, nullptr);
  EXPECT_NEAR(entity->Position().x, 0.5f, 0.001f);
  EXPECT_NEAR(entity->Position().z, 0.0f, 0.001f);
  EXPECT_NEAR(entity->Direction().z, 1.0f, 0.001f);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(735), 0u);
  const auto* state = app_.MovementSystemForTest().state_store().Find(735);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->last_processed_input_seq, 20u);
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCurveAffectsMovementCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(730, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  float samples[] = {0.0f, 0.25f, 1.0f};
  NativeMovementCurve curve;
  curve.curve_id = 12;
  curve.samples = samples;
  curve.sample_count = 3;
  EXPECT_TRUE(app_.NativeProvider()->SetMovementCurve(curve));

  auto command = MakeNativeMovementCommand(94);
  command.curve_id = 12;
  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(730, command));

  app_.TickMovementSystemForTest(0.5f);

  auto* entity = app_.FindRealEntity(730);
  ASSERT_NE(entity, nullptr);
  EXPECT_NEAR(entity->Position().x, 0.25f, 0.001f);
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCommandRejectsUnknownCurve) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(731, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  auto command = MakeNativeMovementCommand(95);
  command.curve_id = 99;

  EXPECT_FALSE(app_.NativeProvider()->SetMovementCommand(731, command));
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(731), nullptr);
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCommandRejectsAllowFullPolicy) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(736, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  auto command = MakeNativeMovementCommand(101);
  command.input_policy =
      static_cast<uint8_t>(movement::MovementCommandInputPolicy::kAllowFull);

  EXPECT_FALSE(app_.NativeProvider()->SetMovementCommand(736, command));
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(736), nullptr);
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCommandKeepsActiveOnInvalidCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(737, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  movement::MovementCommand active;
  active.command_id = 102;
  active.start_position = {0.0f, 0.0f, 0.0f};
  active.target_position = {1.0f, 0.0f, 0.0f};
  active.duration_ms = 1000;
  active.curve_id = 0;
  active.priority = 5;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(737, active));

  auto invalid = MakeNativeMovementCommand(103, 6);
  invalid.input_policy =
      static_cast<uint8_t>(movement::MovementCommandInputPolicy::kAllowFull);

  EXPECT_FALSE(app_.NativeProvider()->SetMovementCommand(737, invalid));
  const auto* stored = app_.MovementSystemForTest().command_store().Find(737);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->command_id, 102u);
  EXPECT_EQ(stored->priority, 5u);
}

TEST_F(CellAppHandlersTest, NativeProviderClearMovementCommandClearsActiveCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(732, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);

  const auto command = MakeNativeMovementCommand(96);
  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(732, command));
  ASSERT_NE(app_.MovementSystemForTest().command_store().Find(732), nullptr);

  EXPECT_TRUE(app_.NativeProvider()->ClearMovementCommand(732, 96));
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(732), nullptr);
  EXPECT_FALSE(app_.NativeProvider()->ClearMovementCommand(732, 96));
}

TEST_F(CellAppHandlersTest, NativeProviderClearMovementCommandHonorsCommandId) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(733, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();

  const auto command = MakeNativeMovementCommand(97);
  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(733, command));

  EXPECT_FALSE(app_.NativeProvider()->ClearMovementCommand(733, 98));
  ASSERT_NE(app_.MovementSystemForTest().command_store().Find(733), nullptr);
  EXPECT_TRUE(app_.NativeProvider()->ClearMovementCommand(733, 0));
  EXPECT_EQ(app_.MovementSystemForTest().command_store().Find(733), nullptr);
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCommandStampsServerTick) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(729, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  app_.GetGameClock().Tick(std::chrono::seconds(1));

  auto command = MakeNativeMovementCommand(93);
  command.server_tick = 999;

  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(729, command));

  const auto* stored = app_.MovementSystemForTest().command_store().Find(729);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->server_tick, app_.GameTime());
}

TEST_F(CellAppHandlersTest, NativeProviderSetMovementCommandHonorsPriority) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(728, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();
  ASSERT_NE(app_.NativeProvider(), nullptr);
  app_.RegisterWatchersForTest();

  movement::MovementCommand active;
  active.command_id = 90;
  active.start_position = {0.0f, 0.0f, 0.0f};
  active.target_position = {1.0f, 0.0f, 0.0f};
  active.duration_ms = 1000;
  active.curve_id = 0;
  active.priority = 5;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(728, active));

  EXPECT_FALSE(app_.NativeProvider()->SetMovementCommand(
      728, MakeNativeMovementCommand(91, 5)));
  auto* stored = app_.MovementSystemForTest().command_store().Find(728);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->command_id, 90u);
  EXPECT_EQ(stored->priority, 5u);
  EXPECT_EQ(app_.GetWatcherRegistry().Get("movement/command_cancelled_total").value_or(""),
            "0");

  EXPECT_TRUE(app_.NativeProvider()->SetMovementCommand(
      728, MakeNativeMovementCommand(92, 6)));
  stored = app_.MovementSystemForTest().command_store().Find(728);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->command_id, 92u);
  EXPECT_EQ(stored->priority, 6u);
  EXPECT_EQ(app_.GetWatcherRegistry().Get("movement/command_started_total").value_or(""),
            "1");
  EXPECT_EQ(app_.GetWatcherRegistry().Get("movement/command_ended_total").value_or(""), "1");
  EXPECT_EQ(app_.GetWatcherRegistry().Get("movement/command_cancelled_total").value_or(""),
            "1");
}

TEST_F(CellAppHandlersTest, NativeProviderSamplesMovementPositionHistory) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(722, 1));
  native_provider_holder_ = app_.CreateNativeProviderForTest();

  movement::MovementState first;
  first.position = {0.0f, 0.0f, 1.0f};
  first.velocity = {0.0f, 0.0f, 3.0f};
  first.direction = {0.0f, 0.0f, 1.0f};
  first.last_processed_input_seq = 30;
  auto second = first;
  second.position = {0.0f, 0.0f, 2.0f};
  second.last_processed_input_seq = 31;
  app_.MovementSystemForTest().position_history().Record(722, 10, first);
  app_.MovementSystemForTest().position_history().Record(722, 12, second);

  NativeMovementHistorySample sample;
  ASSERT_TRUE(native_provider_holder_->TryGetMovementHistorySample(722, 11, sample));
  EXPECT_EQ(sample.server_tick, 11u);
  EXPECT_FLOAT_EQ(sample.position_z, 1.5f);
  EXPECT_EQ(sample.last_processed_input_seq, 31u);
}

TEST_F(CellAppHandlersTest, TickMovementSystemDropsUnsafeStoredState) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(713, 1));
  auto* entity = app_.FindRealEntity(713);
  ASSERT_NE(entity, nullptr);
  auto& state = app_.MovementSystemForTest().state_store().Ensure(
      entity->Id(), entity->Position(), entity->Direction(), entity->OnGround());
  state.velocity = {1000.0f, 0.0f, 0.0f};

  app_.TickMovementSystemForTest(1.0f / 30.0f);

  EXPECT_EQ(app_.MovementSystemForTest().state_store().Find(713), nullptr);
}

TEST_F(CellAppHandlersTest, ClientMovementInputRejectsUntrustedAndCrossEntity) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(701, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(702, 1));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 701;
  msg.target_entity_id = 701;
  msg.frames.push_back({1, 1, 0, 127, 0, 0, 0, 16});

  app_.OnClientMovementInputForward(Address(0x7F000001u, 40000), nullptr, msg);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(701), 0u);

  msg.target_entity_id = 702;
  app_.OnClientMovementInputForward({}, nullptr, msg);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(702), 0u);
}

TEST_F(CellAppHandlersTest, ClientMovementInputDropsUnknownTarget) {
  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 900;
  msg.target_entity_id = 900;
  msg.frames.push_back({1, 1, 0, 127, 0, 0, 0, 16});

  app_.OnClientMovementInputForward({}, nullptr, msg);

  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().TotalQueueDepth(), 0u);
}

TEST_F(CellAppHandlersTest, ClientMovementInputDropsInvalidClientDt) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(704, 1));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 704;
  msg.target_entity_id = 704;
  msg.frames.push_back({1, 1, 0, 127, 0, 0, 0, 0});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(704), 0u);

  msg.frames.clear();
  msg.frames.push_back({2, 2, 0, 127, 0, 0, 0, 1000});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(704), 0u);

  msg.frames.clear();
  msg.frames.push_back({3, 3, 0, 127, 0, 0, 0, 33});
  app_.OnClientMovementInputForward({}, nullptr, msg);
  EXPECT_EQ(app_.MovementSystemForTest().input_buffer().QueueDepth(704), 1u);
}

TEST_F(CellAppHandlersTest, ClientMovementInputRateLimitsPacketBurst) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(703, 1));

  cellapp::ClientMovementInputForward msg;
  msg.source_entity_id = 703;
  msg.target_entity_id = 703;

  for (uint32_t seq = 1; seq <= 32; ++seq) {
    msg.frames.clear();
    msg.frames.push_back({seq, seq, 0, 127, 0, 0, 0, 16});
    app_.OnClientMovementInputForward({}, nullptr, msg);
  }

  EXPECT_LT(app_.MovementSystemForTest().input_buffer().QueueDepth(703), 32u);

  app_.GetGameClock().Tick(std::chrono::seconds(1));
  const auto before = app_.MovementSystemForTest().input_buffer().QueueDepth(703);
  msg.frames.clear();
  msg.frames.push_back({100, 100, 0, 127, 0, 0, 0, 16});
  app_.OnClientMovementInputForward({}, nullptr, msg);

  EXPECT_GT(app_.MovementSystemForTest().input_buffer().QueueDepth(703), before);
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

TEST_F(CellAppHandlersTest, CreateGhostRejectsUnregisteredCellAppSource) {
  const Address untrusted(0x7F000001u, 49000);
  app_.OnCreateGhost(untrusted, FakeChannel(0xBEEF), MakeGhost(501));

  EXPECT_EQ(app_.FindEntity(501), nullptr);
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

  app_.OnPeerCellAppDeath(Address(0x7F000001u, 40001), dying_ch, 1);

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

  app_.HandlePeerLost(dying_addr, false);

  EXPECT_EQ(app_.FindEntity(700), nullptr);
  EXPECT_EQ(rd->HauntCount(), 1u);
  EXPECT_TRUE(rd->HasHaunt(other_ch));

  app_.HandlePeerLost(dying_addr, false);
  EXPECT_EQ(rd->HauntCount(), 1u);
  EXPECT_TRUE(rd->HasHaunt(other_ch));
}

namespace {

auto MakeOwnerSpace(CellApp& app, SpaceID space_id, cellappmgr::CellID primary_cell_id,
                    uint16_t self_port, uint64_t geometry_version = 0) -> Space* {
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
  space->SetBspTree(std::move(tree), geometry_version);
  return space;
}

auto MakeGhostSpace(CellApp& app, SpaceID space_id, cellappmgr::CellID primary_cell_id,
                    cellappmgr::CellID local_cell_id, uint16_t owner_port,
                    uint16_t self_port, uint64_t geometry_version = 0) -> Space* {
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
  space->SetBspTree(std::move(tree), geometry_version);
  app.PeerRegistryForTest().InsertForTest(Address(0x7F000001u, owner_port),
                                          FakeChannel(owner_port));
  return space;
}

}  // namespace

TEST_F(CellAppHandlersTest, FlushLoadReportSendsImmediateReport) {
  InterfaceTable table;
  RecordingChannel mgr_ch(dispatcher_, table, Address(0x7F000001u, 20001));

  cellappmgr::RegisterCellAppAck ack;
  ack.success = true;
  ack.app_id = 7;
  app_.OnRegisterCellAppAck({}, &mgr_ch, ack);
  MakeOwnerSpace(app_, 7, 1, 30001, /*geometry_version=*/12);

  app_.FlushLoadReportForTest();
  auto loads = InformCellLoads(mgr_ch);
  ASSERT_EQ(loads.size(), 1u);
  EXPECT_EQ(loads.back().app_id, 7u);
  ASSERT_EQ(loads.back().cells.size(), 1u);
  EXPECT_EQ(loads.back().cells[0].cell_id, 1u);
  EXPECT_EQ(loads.back().cells[0].geometry_version, 12u);

  app_.FlushLoadReportForTest();
  loads = InformCellLoads(mgr_ch);
  EXPECT_EQ(loads.size(), 2u);
}

TEST_F(CellAppHandlersTest, ReportScriptTickContributesToCellLoadReport) {
  InterfaceTable table;
  RecordingChannel mgr_ch(dispatcher_, table, Address(0x7F000001u, 20001));

  cellappmgr::RegisterCellAppAck ack;
  ack.success = true;
  ack.app_id = 7;
  app_.OnRegisterCellAppAck({}, &mgr_ch, ack);
  MakeOwnerSpace(app_, 7, 1, 30001, /*geometry_version=*/12);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(900, 7));

  native_provider_holder_ = app_.CreateNativeProviderForTest();
  app_.NativeProvider()->ReportScriptTick(900, 20000);

  app_.FlushLoadReportForTest();
  const auto loads = InformCellLoads(mgr_ch);
  ASSERT_EQ(loads.size(), 1u);
  ASSERT_EQ(loads.back().cells.size(), 1u);
  EXPECT_EQ(loads.back().cells[0].script_tick_us, 20000u);
  EXPECT_EQ(loads.back().cells[0].native_tick_us, 0u);
  EXPECT_EQ(loads.back().cells[0].x_load_buckets[4], 20000u);
  EXPECT_EQ(loads.back().cells[0].z_load_buckets[4], 20000u);
  EXPECT_GT(loads.back().cells[0].tick_load, 0.f);
}

TEST_F(CellAppHandlersTest, FailedInformCellLoadPreservesTickCountersForRetry) {
  InterfaceTable table;
  RecordingChannel mgr_ch(dispatcher_, table, Address(0x7F000001u, 20001));

  cellappmgr::RegisterCellAppAck ack;
  ack.success = true;
  ack.app_id = 7;
  app_.OnRegisterCellAppAck({}, &mgr_ch, ack);
  MakeOwnerSpace(app_, 7, 1, 30001, /*geometry_version=*/12);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(900, 7));

  native_provider_holder_ = app_.CreateNativeProviderForTest();
  app_.NativeProvider()->ReportScriptTick(900, 20000);

  app_.RegisterWatchersForTest();
  EXPECT_EQ(app_.GetWatcherRegistry()
                .Get("cellapp/inform_cell_load_send_failures_total")
                .value_or(""),
            "0");
  mgr_ch.FailNextSend();
  app_.FlushLoadReportForTest();
  EXPECT_TRUE(InformCellLoads(mgr_ch).empty());
  EXPECT_EQ(app_.GetWatcherRegistry()
                .Get("cellapp/inform_cell_load_send_failures_total")
                .value_or(""),
            "1");

  app_.FlushLoadReportForTest();
  const auto loads = InformCellLoads(mgr_ch);
  ASSERT_EQ(loads.size(), 1u);
  ASSERT_EQ(loads.back().cells.size(), 1u);
  EXPECT_EQ(loads.back().cells[0].script_tick_us, 20000u);
  EXPECT_EQ(loads.back().cells[0].x_load_buckets[4], 20000u);
}

TEST_F(CellAppHandlersTest, CellAppMgrBirthReconnectsOnlyForNewManager) {
  InterfaceTable table;
  RecordingChannel mgr_ch(dispatcher_, table, Address(0x7F000001u, 25001));
  app_.SeedCellAppMgrSessionForTest(&mgr_ch, 7, 101);

  machined::BirthNotification birth;
  birth.process_type = ProcessType::kCellAppMgr;
  birth.name = "cellappmgr";
  birth.internal_addr = mgr_ch.RemoteAddress();
  birth.pid = 101;
  EXPECT_FALSE(app_.ShouldReconnectCellAppMgrForBirthForTest(birth));

  auto new_pid = birth;
  new_pid.pid = 102;
  EXPECT_TRUE(app_.ShouldReconnectCellAppMgrForBirthForTest(new_pid));

  auto moved = birth;
  moved.internal_addr = Address(0x7F000001u, 25002);
  EXPECT_TRUE(app_.ShouldReconnectCellAppMgrForBirthForTest(moved));

  auto legacy = birth;
  legacy.pid = 0;
  EXPECT_TRUE(app_.ShouldReconnectCellAppMgrForBirthForTest(legacy));

  machined::DeathNotification other_death;
  other_death.process_type = ProcessType::kCellAppMgr;
  other_death.name = "cellappmgr-old";
  other_death.internal_addr = Address(0x7F000001u, 25002);
  other_death.reason = 1;
  app_.OnCellAppMgrDeathForTest(other_death);
  EXPECT_EQ(app_.AppId(), 7u);
  EXPECT_EQ(app_.CellAppMgrPidForTest(), 101u);

  app_.OnOutboundChannelDeath(mgr_ch);
  EXPECT_EQ(app_.AppId(), 0u);
  EXPECT_EQ(app_.CellAppMgrPidForTest(), 0u);
  EXPECT_TRUE(app_.ShouldReconnectCellAppMgrForBirthForTest(birth));
}

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

TEST_F(CellAppHandlersTest, OnSpaceDataUpdateRejectsUnregisteredCellAppSource) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002);

  cellapp::SpaceDataUpdate msg;
  msg.space_id = 7;
  msg.key_id = 42;
  msg.value = {0xAA, 0xBB};
  app_.OnSpaceDataUpdate(Address(0x7F000001u, 49000), nullptr, msg);

  EXPECT_FALSE(space->Data().Contains(42));
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

TEST_F(CellAppHandlersTest, PrimaryHandoffAddCellAcksAfterSpaceDataSnapshot) {
  InterfaceTable table;
  RecordingChannel owner_ch(dispatcher_, table, Address(0x7F000001u, 30001));
  RecordingChannel mgr_ch(dispatcher_, table, Address(0x7F000001u, 20001));
  app_.PeerRegistryForTest().InsertForTest(Address(0x7F000001u, 30001), &owner_ch);

  cellappmgr::AddCellToSpace add;
  add.space_id = 7;
  add.cell_id = 1;
  add.bounds = {-100.f, -100.f, 100.f, 100.f};
  add.is_primary = true;
  add.space_data_source_addr = Address(0x7F000001u, 30001);
  app_.OnAddCellToSpace({}, &mgr_ch, add);

  auto* space = app_.FindSpace(7);
  ASSERT_NE(space, nullptr);
  EXPECT_FALSE(space->IsDataInitialized());
  const auto requests = SpaceDataSnapshotRequests(owner_ch);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].space_id, 7u);
  EXPECT_TRUE(AddCellToSpaceAcks(mgr_ch).empty());

  cellapp::SpaceDataSnapshot snap;
  snap.space_id = 7;
  snap.entries.push_back({42, {0xAA, 0xBB}});
  app_.OnSpaceDataSnapshot(Address(0x7F000001u, 30001), nullptr, snap);

  EXPECT_TRUE(space->IsDataInitialized());
  ASSERT_NE(space->Data().Get(42), nullptr);
  EXPECT_EQ(*space->Data().Get(42), (std::vector<uint8_t>{0xAA, 0xBB}));
  const auto acks = AddCellToSpaceAcks(mgr_ch);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].space_id, 7u);
  EXPECT_EQ(acks[0].cell_id, 1u);
}

TEST_F(CellAppHandlersTest, OnSpaceDataSnapshotRequest_NonOwnerIsNoop) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002);
  ASSERT_FALSE(space->IsOwner());

  cellapp::SpaceDataSnapshotRequest req;
  req.space_id = 7;
  // Non-owner should silently drop - no reply path attempted (would crash
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

TEST_F(CellAppHandlersTest, ShouldOffloadIgnoresStaleFreezeEpoch) {
  auto* space = MakeOwnerSpace(app_, 7, 1, 30001);
  auto* cell = space->FindLocalCell(1);
  ASSERT_NE(cell, nullptr);

  cellappmgr::ShouldOffload freeze;
  freeze.space_id = 7;
  freeze.cell_id = 1;
  freeze.enable = false;
  freeze.freeze_epoch = 2;
  app_.OnShouldOffload({}, nullptr, freeze);
  EXPECT_FALSE(cell->ShouldOffload());

  cellappmgr::ShouldOffload stale_enable = freeze;
  stale_enable.enable = true;
  stale_enable.freeze_epoch = 1;
  app_.OnShouldOffload({}, nullptr, stale_enable);
  EXPECT_FALSE(cell->ShouldOffload());

  cellappmgr::ShouldOffload enable = freeze;
  enable.enable = true;
  app_.OnShouldOffload({}, nullptr, enable);
  EXPECT_TRUE(cell->ShouldOffload());
}

TEST_F(CellAppHandlersTest, RemoveCellFromSpaceRemovesEmptyLocalCell) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002);
  ASSERT_NE(space->FindLocalCell(2), nullptr);

  cellappmgr::RemoveCellFromSpace msg;
  msg.space_id = 7;
  msg.cell_id = 2;
  app_.OnRemoveCellFromSpace({}, nullptr, msg);

  EXPECT_EQ(space->FindLocalCell(2), nullptr);
}

TEST_F(CellAppHandlersTest, AddCellToSpaceBackfillsExistingRealMembership) {
  cellapp::CreateSpace cs;
  cs.space_id = 7;
  app_.OnCreateSpace({}, nullptr, cs);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(7007, 7, {3, 0, 3}));
  auto* real = app_.FindRealEntity(7007);
  ASSERT_NE(real, nullptr);

  cellappmgr::AddCellToSpace add;
  add.space_id = 7;
  add.cell_id = 3;
  app_.OnAddCellToSpace({}, nullptr, add);

  auto* space = app_.FindSpace(7);
  ASSERT_NE(space, nullptr);
  auto* cell = space->FindLocalCell(3);
  ASSERT_NE(cell, nullptr);
  EXPECT_TRUE(cell->HasRealEntity(real));
}

TEST_F(CellAppHandlersTest, OffloadEntityRejectsMismatchedGeometryVersion) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7777;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1, 0, 1};
  msg.direction = {1, 0, 0};
  msg.target_cell_id = 2;
  msg.geometry_version = 4;

  app_.OnOffloadEntity({}, nullptr, msg);
  EXPECT_EQ(app_.FindRealEntity(7777), nullptr);
}

TEST_F(CellAppHandlersTest, OffloadEntityRejectsUnregisteredCellAppSource) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7778;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1, 0, 1};
  msg.direction = {1, 0, 0};
  msg.target_cell_id = 2;
  msg.geometry_version = 5;

  app_.OnOffloadEntity(Address(0x7F000001u, 49000), FakeChannel(0xBEEF), msg);
  EXPECT_EQ(app_.FindRealEntity(7778), nullptr);
}

TEST_F(CellAppHandlersTest, OffloadEntityRejectsMissingTargetCell) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7777;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1, 0, 1};
  msg.direction = {1, 0, 0};
  msg.target_cell_id = 99;
  msg.geometry_version = 5;

  app_.OnOffloadEntity({}, nullptr, msg);
  EXPECT_EQ(app_.FindRealEntity(7777), nullptr);
}

TEST_F(CellAppHandlersTest, OffloadEntityAcceptsMatchingGeometryTarget) {
  auto* space = MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);
  auto* cell = space->FindLocalCell(2);
  ASSERT_NE(cell, nullptr);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7777;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1, 0, 1};
  msg.direction = {1, 0, 0};
  msg.target_cell_id = 2;
  msg.geometry_version = 5;

  app_.OnOffloadEntity({}, nullptr, msg);
  auto* real = app_.FindRealEntity(7777);
  ASSERT_NE(real, nullptr);
  EXPECT_TRUE(cell->HasRealEntity(real));
}

TEST_F(CellAppHandlersTest, OffloadEntityRestoresMovementState) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7779;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1.0f, 0.0f, 1.0f};
  msg.direction = {0.0f, 0.0f, 1.0f};
  msg.target_cell_id = 2;
  msg.geometry_version = 5;
  msg.has_movement_state = true;
  msg.movement_state.position = msg.position;
  msg.movement_state.velocity = {0.0f, 0.0f, 3.0f};
  msg.movement_state.direction = msg.direction;
  msg.movement_state.flags = movement::kMovementFlagGrounded;
  msg.movement_state.last_processed_input_seq = 77;

  app_.OnOffloadEntity({}, nullptr, msg);

  const auto* state = app_.MovementSystemForTest().state_store().Find(7779);
  ASSERT_NE(state, nullptr);
  EXPECT_FLOAT_EQ(state->velocity.z, 3.0f);
  EXPECT_EQ(state->last_processed_input_seq, 77u);
}

TEST_F(CellAppHandlersTest, OffloadEntityRestoresMovementPositionHistory) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7781;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1.0f, 0.0f, 1.0f};
  msg.direction = {0.0f, 0.0f, 1.0f};
  msg.target_cell_id = 2;
  msg.geometry_version = 5;

  movement::MovementState first;
  first.position = {1.0f, 0.0f, 1.5f};
  first.velocity = {0.0f, 0.0f, 2.0f};
  first.direction = msg.direction;
  first.last_processed_input_seq = 10;
  movement::MovementState second = first;
  second.position = {1.0f, 0.0f, 2.0f};
  second.last_processed_input_seq = 11;
  // Source ticks (30, 31) get rebased so the latest sits at dest's
  // MovementServerTick(); samples that would go negative after rebase
  // are dropped (older lag-comp data older than dest has been alive).
  msg.movement_position_history.push_back(MovementPositionSample{30, first});
  msg.movement_position_history.push_back(MovementPositionSample{31, second});

  app_.OnOffloadEntity({}, nullptr, msg);

  const auto dest_tick = app_.MovementServerTickForTest();
  const auto* history = app_.MovementSystemForTest().position_history().Find(7781);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), dest_tick >= 1u ? 2u : 1u);
  EXPECT_EQ(history->back().server_tick, dest_tick);
  EXPECT_FLOAT_EQ(history->back().state.position.z, 2.0f);
  if (history->size() == 2u) {
    EXPECT_EQ(history->front().server_tick, dest_tick - 1u);
    EXPECT_FLOAT_EQ(history->front().state.position.z, 1.5f);
  }
}

TEST_F(CellAppHandlersTest, OffloadEntityRestoresMovementCommand) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7782;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1.0f, 0.0f, 1.0f};
  msg.direction = {0.0f, 0.0f, 1.0f};
  msg.target_cell_id = 2;
  msg.geometry_version = 5;
  msg.has_movement_command = true;
  msg.movement_command.command_id = 91;
  msg.movement_command.start_position = msg.position;
  msg.movement_command.target_position = {1.0f, 0.0f, 4.0f};
  msg.movement_command.duration_ms = 600;
  msg.movement_command.elapsed_ms = 120;
  msg.movement_command.curve_id = 3;

  app_.OnOffloadEntity({}, nullptr, msg);

  const auto* command = app_.MovementSystemForTest().command_store().Find(7782);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command_id, 91u);
  EXPECT_EQ(command->elapsed_ms, 120u);
  EXPECT_FLOAT_EQ(command->target_position.z, 4.0f);
}

TEST_F(CellAppHandlersTest, OffloadEntityDropsUnsafeMovementState) {
  MakeGhostSpace(app_, 7, 1, 2, 30001, 30002, /*geometry_version=*/5);

  cellapp::OffloadEntity msg;
  msg.entity_id = 7780;
  msg.type_id = 1;
  msg.space_id = 7;
  msg.position = {1.0f, 0.0f, 1.0f};
  msg.direction = {0.0f, 0.0f, 1.0f};
  msg.target_cell_id = 2;
  msg.geometry_version = 5;
  msg.has_movement_state = true;
  msg.movement_state.position = msg.position;
  msg.movement_state.velocity = {1000.0f, 0.0f, 0.0f};
  msg.movement_state.direction = msg.direction;
  msg.movement_state.flags = movement::kMovementFlagGrounded;

  app_.OnOffloadEntity({}, nullptr, msg);

  EXPECT_EQ(app_.MovementSystemForTest().state_store().Find(7780), nullptr);
}

TEST_F(CellAppHandlersTest, BuildOffloadMessageCapturesPersistentBlob) {
  EnableGhostLifecycleCallbacks(/*with_serialize=*/true);
  std::vector<std::byte> blob{std::byte{0x77}, std::byte{0x88}};
  g_serialize_blob = &blob;

  app_.OnCreateCellEntity({}, nullptr, MakeCreate(910, 1, {1, 0, 1}));
  auto* real = app_.FindRealEntity(910);
  ASSERT_NE(real, nullptr);

  auto msg = app_.BuildOffloadMessage(*real);

  ASSERT_EQ(msg.persistent_blob.size(), 2u);
  EXPECT_EQ(msg.persistent_blob[0], std::byte{0x77});
  EXPECT_EQ(msg.persistent_blob[1], std::byte{0x88});
}

TEST_F(CellAppHandlersTest, BuildOffloadMessageCapturesMovementState) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(911, 1, {1, 0, 1}));
  auto* real = app_.FindRealEntity(911);
  ASSERT_NE(real, nullptr);
  auto& state = app_.MovementSystemForTest().state_store().Ensure(
      real->Id(), real->Position(), real->Direction(), real->OnGround());
  state.velocity = {0.0f, 0.0f, 2.5f};
  state.last_processed_input_seq = 55;

  auto msg = app_.BuildOffloadMessage(*real);

  EXPECT_TRUE(msg.has_movement_state);
  EXPECT_FLOAT_EQ(msg.movement_state.velocity.z, 2.5f);
  EXPECT_EQ(msg.movement_state.last_processed_input_seq, 55u);
}

TEST_F(CellAppHandlersTest, BuildOffloadMessageCapturesMovementPositionHistory) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(912, 1, {1, 0, 1}));
  auto* real = app_.FindRealEntity(912);
  ASSERT_NE(real, nullptr);

  movement::MovementState first;
  first.position = {1.0f, 0.0f, 1.5f};
  first.velocity = {0.0f, 0.0f, 1.0f};
  first.direction = real->Direction();
  first.last_processed_input_seq = 21;
  movement::MovementState second = first;
  second.position = {1.0f, 0.0f, 2.0f};
  second.last_processed_input_seq = 22;
  app_.MovementSystemForTest().position_history().Record(real->Id(), 40, first);
  app_.MovementSystemForTest().position_history().Record(real->Id(), 41, second);

  auto msg = app_.BuildOffloadMessage(*real);

  ASSERT_EQ(msg.movement_position_history.size(), 2u);
  EXPECT_EQ(msg.movement_position_history[0].server_tick, 40u);
  EXPECT_FLOAT_EQ(msg.movement_position_history[1].state.position.z, 2.0f);
}

TEST_F(CellAppHandlersTest, BuildOffloadMessageCapturesMovementCommand) {
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(913, 1, {1, 0, 1}));
  auto* real = app_.FindRealEntity(913);
  ASSERT_NE(real, nullptr);

  movement::MovementCommand command;
  command.command_id = 13;
  command.start_position = real->Position();
  command.target_position = {3.0f, 0.0f, 1.0f};
  command.duration_ms = 800;
  command.elapsed_ms = 200;
  command.curve_id = 5;
  ASSERT_TRUE(app_.MovementSystemForTest().command_store().Set(real->Id(), command));

  auto msg = app_.BuildOffloadMessage(*real);

  EXPECT_TRUE(msg.has_movement_command);
  EXPECT_EQ(msg.movement_command.command_id, 13u);
  EXPECT_EQ(msg.movement_command.elapsed_ms, 200u);
  EXPECT_FLOAT_EQ(msg.movement_command.target_position.x, 3.0f);
}

// Ghost lifecycle wire-up tests verify callbacks at transition points.
// They also check ordering against Real-side restore and migrate callbacks.
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

TEST_F(CellAppHandlersTest, RevertPendingOffloadRestoresMovementState) {
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(901));

  CellApp::PendingOffload po;
  po.target_addr = Address{0x7F000001u, 26002};
  po.sent_at = std::chrono::steady_clock::now();
  po.space_id = 1;
  po.type_id = 1;
  po.has_movement_state = true;
  po.movement_state.position = {1.0f, 0.0f, 2.0f};
  po.movement_state.velocity = {0.0f, 0.0f, 4.0f};
  po.movement_state.direction = {0.0f, 0.0f, 1.0f};
  po.movement_state.flags = movement::kMovementFlagGrounded;
  po.movement_state.last_processed_input_seq = 91;
  app_.PendingOffloadsForTest()[901] = std::move(po);

  app_.RevertPendingOffload(901, "test");

  const auto* state = app_.MovementSystemForTest().state_store().Find(901);
  ASSERT_NE(state, nullptr);
  EXPECT_FLOAT_EQ(state->velocity.z, 4.0f);
  EXPECT_EQ(state->last_processed_input_seq, 91u);
}

TEST_F(CellAppHandlersTest, RevertPendingOffloadRestoresMovementPositionHistory) {
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(902));

  movement::MovementState sample_state;
  sample_state.position = {1.0f, 0.0f, 2.0f};
  sample_state.velocity = {0.0f, 0.0f, 3.0f};
  sample_state.direction = {0.0f, 0.0f, 1.0f};
  sample_state.last_processed_input_seq = 92;

  CellApp::PendingOffload po;
  po.target_addr = Address{0x7F000001u, 26002};
  po.sent_at = std::chrono::steady_clock::now();
  po.space_id = 1;
  po.type_id = 1;
  po.movement_position_history.push_back(MovementPositionSample{50, sample_state});
  app_.PendingOffloadsForTest()[902] = std::move(po);

  app_.RevertPendingOffload(902, "test");

  const auto* history = app_.MovementSystemForTest().position_history().Find(902);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ((*history)[0].server_tick, 50u);
  EXPECT_FLOAT_EQ((*history)[0].state.velocity.z, 3.0f);
}

TEST_F(CellAppHandlersTest, RevertPendingOffloadRestoresMovementCommand) {
  app_.OnCreateGhost({}, FakeChannel(0xBEEF), MakeGhost(903));

  CellApp::PendingOffload po;
  po.target_addr = Address{0x7F000001u, 26002};
  po.sent_at = std::chrono::steady_clock::now();
  po.space_id = 1;
  po.type_id = 1;
  po.has_movement_command = true;
  po.movement_command.command_id = 51;
  po.movement_command.start_position = {1.0f, 0.0f, 1.0f};
  po.movement_command.target_position = {1.0f, 0.0f, 6.0f};
  po.movement_command.duration_ms = 700;
  po.movement_command.elapsed_ms = 350;
  po.movement_command.curve_id = 2;
  app_.PendingOffloadsForTest()[903] = std::move(po);

  app_.RevertPendingOffload(903, "test");

  const auto* command = app_.MovementSystemForTest().command_store().Find(903);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command_id, 51u);
  EXPECT_EQ(command->elapsed_ms, 350u);
  EXPECT_FLOAT_EQ(command->target_position.z, 6.0f);
}

TEST_F(CellAppHandlersTest, PeerCellAppDeathFiresDestroyGhostForOrphans) {
  EnableGhostLifecycleCallbacks();
  Channel* dying = FakeChannel(0xDEAD);
  app_.OnCreateGhost({}, dying, MakeGhost(800));
  ghost_calls_.clear();

  app_.OnPeerCellAppDeath(Address{0x7F000001u, 26002}, dying, 1);

  ASSERT_EQ(ghost_calls_.size(), 1u);
  EXPECT_EQ(ghost_calls_[0].kind, GhostCall::kDestroyGhost);
  EXPECT_EQ(ghost_calls_[0].entity_id, 800u);
}

TEST_F(CellAppHandlersTest, MgrMessageFromForeignChannelDropped) {
  // After registering with one CellAppMgr channel, control messages arriving
  // on a different channel (a straggler from a dead mgr after takeover) are
  // dropped — the BigWorld "only obey the current mgr" guard.
  InterfaceTable table;
  RecordingChannel mgr_ch(dispatcher_, table, Address(0x7F000001u, 20001));
  RecordingChannel rogue_ch(dispatcher_, table, Address(0x7F000001u, 20002));
  cellappmgr::RegisterCellAppAck ack;
  ack.success = true;
  ack.app_id = 7;
  app_.OnRegisterCellAppAck({}, &mgr_ch, ack);

  cellappmgr::AddCellToSpace add;
  add.space_id = 99;
  add.cell_id = 1;
  app_.OnAddCellToSpace({}, &rogue_ch, add);

  EXPECT_EQ(app_.FindSpace(99), nullptr);
  EXPECT_EQ(app_.CellAppMgrStaleDrops(), 1u);
}

}  // namespace
}  // namespace atlas
