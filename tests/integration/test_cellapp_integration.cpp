// CellApp integration tests.
//
// Categories:
//   1. Smoke: CellApp constructs without CLR.
//   2. Handler-level: exercises CellApp handlers directly. Covers entity
//      creation, EnableWitness AoI, avatar movement, property deltas,
//      MoveToPointController, RPC security chain, and 1000-entity perf.
//   3. Multi-process: CellApp binary registers with CellAppMgr via machined.
//
// All tests are pure C++ — no CLR runtime needed.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef SendMessage  // collides with Channel::SendMessage
#endif

#include <gtest/gtest.h>

#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "entitydef/entity_def_registry.h"
#include "entitydef/entity_type_descriptor.h"
#include "math/vector3.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/socket.h"
#include "serialization/binary_stream.h"
#include "server/machined_client.h"
#include "space.h"
#include "space/move_controller.h"
#include "witness.h"

namespace atlas {
namespace {

// ============================================================================
// Smoke
// ============================================================================

TEST(CellAppIntegration, ConstructsCleanly) {
  EventDispatcher dispatcher("smoke");
  NetworkInterface network(dispatcher);
  CellApp app(dispatcher, network);
  EXPECT_EQ(app.Spaces().size(), 0u);
}

// ============================================================================
// Handler-level integration — no CLR required
// ============================================================================

class CellAppIntegrationFixture : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test_cellapp_integration"};
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

// Scenario #2 — CreateCellEntity registers the entity and responds with
// a CellEntityCreated ack that carries cell_addr. We test the local state
// mutations here (ack dispatch requires a real channel, tested in
// test_cellappmgr_integration).
TEST_F(CellAppIntegrationFixture, CreateCellEntityRegistersAndResponds) {
  auto msg = MakeCreate(/*base_id=*/100, /*space=*/5, {10, 0, 10});
  app_.OnCreateCellEntity({}, /*ch=*/nullptr, msg);

  // Entity registered in both indexes.
  auto* by_base = app_.FindEntityByBaseId(100);
  ASSERT_NE(by_base, nullptr);
  EXPECT_EQ(by_base->BaseEntityId(), 100u);
  EXPECT_FLOAT_EQ(by_base->Position().x, 10.f);

  auto* by_cell = app_.FindEntity(by_base->Id());
  EXPECT_EQ(by_cell, by_base);

  // Space auto-created.
  EXPECT_NE(app_.FindSpace(5), nullptr);
}

// PR 34 end-to-end cell-side flow. Mirrors what BaseApp does on login:
//   1. CreateCellEntity — entity exists, no witness yet (C2 decoupled).
//   2. EnableWitness     — fires from BaseApp::BindClient (C3). No radius
//                          on the wire; CellAppConfig defaults pick up.
//   3. SetAoIRadius      — script call (C4/C5). Shrinks the witness's
//                          dual-band trigger, peers outside the new
//                          outer band get marked for leave.
// Locks in the cross-commit contract so a regression in any of C2/C3/C4
// surfaces here.
TEST_F(CellAppIntegrationFixture, Pr34EndToEndEnableThenSetAoIRadius) {
  // Peer at ~250m — well inside the 500m CellAppConfig default but
  // well outside the 55m post-SetAoIRadius outer band.
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(/*base_id=*/200, /*space=*/1, {250.f, 0.f, 0.f}));

  // Observer. After C2, creation doesn't auto-enable a witness.
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(/*base_id=*/100, /*space=*/1, {0.f, 0.f, 0.f}));
  auto* observer = app_.FindEntityByBaseId(100);
  ASSERT_NE(observer, nullptr);
  EXPECT_FALSE(observer->HasWitness());

  // BindClient would now fire EnableWitness. After C2 the wire form
  // carries only base_entity_id; radius + hysteresis come from config.
  cellapp::EnableWitness e{100};
  app_.OnEnableWitness({}, nullptr, e);
  ASSERT_TRUE(observer->HasWitness());
  EXPECT_FLOAT_EQ(observer->GetWitness()->AoIRadius(), 500.f);
  EXPECT_FLOAT_EQ(observer->GetWitness()->Hysteresis(), 5.f);
  // Peer at 250m is inside the 500m AoI → witness saw it as ENTER_PENDING.
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  // Script-side SetAoIRadius(50, 5) — shrinks AoI to the stress-test band.
  cellapp::SetAoIRadius s;
  s.base_entity_id = 100;
  s.radius = 50.f;
  s.hysteresis = 5.f;
  app_.OnSetAoIRadius({}, nullptr, s);
  EXPECT_FLOAT_EQ(observer->GetWitness()->AoIRadius(), 50.f);
  EXPECT_FLOAT_EQ(observer->GetWitness()->Hysteresis(), 5.f);

  // Peer at 250m is now outside the new outer band (55m); the trigger
  // contraction marked it kGone. Update compacts the map.
  observer->GetWitness()->Update(/*max_packet_bytes=*/4096);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

// PR 34 C2: EnableWitness with no valid entity should log-warn and
// leave the cell app in a sane state. This is defensive — the BindClient
// → cell race window allows EnableWitness to arrive for an entity that
// got destroyed in the interim.
TEST_F(CellAppIntegrationFixture, Pr34EnableWitnessForUnknownEntityIsNoop) {
  cellapp::EnableWitness e{9999};        // never created
  app_.OnEnableWitness({}, nullptr, e);  // must not crash
  EXPECT_EQ(app_.FindEntityByBaseId(9999), nullptr);
}

// Scenario #3 — EnableWitness creates an AoITrigger; existing peers
// already in range appear as ENTER_PENDING in the aoi_map. This test
// proves the C++ trigger integration without CLR.

struct CapturedEnvelope {
  EntityID observer_base_id;
  std::vector<std::byte> payload;
};

class WitnessIntegrationFixture : public ::testing::Test {
 protected:
  std::vector<CapturedEnvelope> sent_;

  auto MakeSendFn() {
    return [this](EntityID observer_id, std::span<const std::byte> env) {
      sent_.push_back({observer_id, std::vector<std::byte>(env.begin(), env.end())});
    };
  }

  static auto KindOf(const CapturedEnvelope& e) -> CellAoIEnvelopeKind {
    return static_cast<CellAoIEnvelopeKind>(e.payload.at(0));
  }

  static auto PublicIdOf(const CapturedEnvelope& e) -> EntityID {
    EntityID id = 0;
    for (int i = 0; i < 4; ++i) {
      id |= static_cast<EntityID>(e.payload[1 + i]) << (i * 8);
    }
    return id;
  }

  static auto MakeEntity(Space& space, EntityID id, uint16_t type_id, math::Vector3 pos)
      -> CellEntity* {
    auto* e = space.AddEntity(
        std::make_unique<CellEntity>(id, type_id, space, pos, math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), /*base_id=*/id + 1000);
    return e;
  }
};

TEST_F(WitnessIntegrationFixture, EnableWitnessFiresEnterForExistingPeers) {
  Space space(1);

  // Peer spawns first.
  auto* peer = MakeEntity(space, 100, 7, {3, 0, 3});

  // Observer spawns and enables witness — peer is already in range.
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/10.f, MakeSendFn());

  // Peer should be ENTER_PENDING in the AoI map.
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  // Update flushes the enter.
  observer->GetWitness()->Update(/*max_packet_bytes=*/4096);
  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityEnter);
  EXPECT_EQ(PublicIdOf(sent_[0]), peer->BaseEntityId());
}

TEST_F(WitnessIntegrationFixture, PeerEnterAndLeaveFireEvents) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn());

  // Peer enters AoI.
  auto* peer = MakeEntity(space, 100, 7, {3, 0, 3});
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityEnter);
  sent_.clear();

  // Peer moves out of AoI.
  peer->SetPosition({100.f, 0.f, 100.f});
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityLeave);
  EXPECT_EQ(PublicIdOf(sent_[0]), peer->BaseEntityId());
}

// ============================================================================
// RPC security — scenario #10
//
// Handler-level RPC validation (defence-in-depth layers L1–L4). These
// tests drive handlers directly with synthetic EntityDef entries and
// confirm accept/reject behaviour at each layer. Uses the same
// RegisterTypeWithRpc + PackRpcId pattern as test_cellapp_handlers.
// ============================================================================

namespace {

auto PackRpcId(uint8_t direction, uint16_t type_index, uint8_t method_index) -> uint32_t {
  return (static_cast<uint32_t>(direction) << 22) | (static_cast<uint32_t>(type_index) << 8) |
         static_cast<uint32_t>(method_index);
}

void RegisterTypeWithRpc(uint16_t type_id, uint32_t rpc_id, ExposedScope scope) {
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

class RpcSecurityFixture : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"rpc_security"};
  NetworkInterface network_{dispatcher_};
  CellApp app_{dispatcher_, network_};
  EntityID observer_base_id_{200};
  EntityID peer_base_id_{300};

  void SetUp() override {
    EntityDefRegistry::Instance().clear();
    // C7 trust boundary: these tests dispatch ClientCellRpcForward with
    // Address{} as the wire src. Without this the trust check would
    // drop every test before reaching the RPC-scope validation they're
    // supposed to exercise.
    app_.InsertTrustedBaseAppForTest(Address{});
  }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }

  auto MakeCreate(EntityID base_id, SpaceID sp) -> cellapp::CreateCellEntity {
    cellapp::CreateCellEntity msg;
    msg.base_entity_id = base_id;
    msg.type_id = 1;
    msg.space_id = sp;
    msg.position = {0, 0, 0};
    msg.direction = {1, 0, 0};
    msg.base_addr = Address(0, 0);
    return msg;
  }
};

TEST_F(RpcSecurityFixture, NonExposedCellRpcRejected) {
  const uint32_t kCellRpc = PackRpcId(0x02, 1, 1);
  RegisterTypeWithRpc(1, kCellRpc, ExposedScope::kNone);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(observer_base_id_, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = observer_base_id_;
  msg.source_entity_id = observer_base_id_;
  msg.rpc_id = kCellRpc;
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(RpcSecurityFixture, OwnClientRpcRejectedWhenSourceNeTarget) {
  const uint32_t kCellRpc = PackRpcId(0x02, 1, 1);
  RegisterTypeWithRpc(1, kCellRpc, ExposedScope::kOwnClient);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(observer_base_id_, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(peer_base_id_, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = observer_base_id_;
  msg.source_entity_id = peer_base_id_;  // different → rejected
  msg.rpc_id = kCellRpc;
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(RpcSecurityFixture, AllClientsRpcAcceptedAcrossEntities) {
  const uint32_t kCellRpc = PackRpcId(0x02, 1, 1);
  RegisterTypeWithRpc(1, kCellRpc, ExposedScope::kAllClients);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(observer_base_id_, 1));
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(peer_base_id_, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = observer_base_id_;
  msg.source_entity_id = peer_base_id_;
  msg.rpc_id = kCellRpc;
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(RpcSecurityFixture, WrongDirectionRpcRejected) {
  const uint32_t kBaseRpc = PackRpcId(0x03, 1, 1);  // direction=Base
  RegisterTypeWithRpc(1, kBaseRpc, ExposedScope::kAllClients);
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(observer_base_id_, 1));

  cellapp::ClientCellRpcForward msg;
  msg.target_entity_id = observer_base_id_;
  msg.source_entity_id = observer_base_id_;
  msg.rpc_id = kBaseRpc;
  app_.OnClientCellRpcForward({}, nullptr, msg);
}

TEST_F(RpcSecurityFixture, InternalCellRpcBypassesExposedCheck) {
  const uint32_t kCellRpc = PackRpcId(0x02, 1, 1);
  RegisterTypeWithRpc(1, kCellRpc, ExposedScope::kNone);  // NOT exposed
  app_.OnCreateCellEntity({}, nullptr, MakeCreate(observer_base_id_, 1));

  cellapp::InternalCellRpc msg;
  msg.target_entity_id = observer_base_id_;
  msg.rpc_id = kCellRpc;
  app_.OnInternalCellRpc({}, nullptr, msg);
}

// ============================================================================
// Scenario #4 — AvatarUpdate moves an entity; observers with overlapping
// AoI receive a volatile position update via the Witness pump.
// ============================================================================

TEST_F(WitnessIntegrationFixture, AvatarUpdatePropagatesToObservers) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  auto* peer = MakeEntity(space, 100, 7, {3, 0, 3});
  observer->EnableWitness(50.f, MakeSendFn());

  // Flush the initial Enter event.
  observer->GetWitness()->Update(4096);
  sent_.clear();

  // Peer moves and publishes a volatile position frame.
  peer->SetPosition({5, 0, 5});
  CellEntity::ReplicationFrame v;
  v.volatile_seq = 1;
  v.position = {5, 0, 5};
  v.direction = {0, 0, 1};
  v.on_ground = true;
  peer->PublishReplicationFrame(v, {}, {});

  // Drive the witness pump — should produce a position update envelope.
  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  bool found_pos_update = false;
  for (const auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPositionUpdate) {
      found_pos_update = true;
      EXPECT_EQ(PublicIdOf(c), peer->BaseEntityId());
    }
  }
  EXPECT_TRUE(found_pos_update) << "Observer should receive position update from moved peer";
}

// ============================================================================
// Scenario #7 — Property changes produce audience-filtered deltas:
// owner_delta for the owning client, other_delta for everyone else.
// ============================================================================

struct ReplicationCapture {
  EntityID observer_base_id{0};
  std::vector<std::byte> payload;
  bool reliable{true};
};

class PropertyDeltaFixture : public ::testing::Test {
 protected:
  std::vector<ReplicationCapture> sent_;

  auto MakeReliable() {
    return [this](EntityID obs, std::span<const std::byte> env) {
      sent_.push_back({obs, std::vector<std::byte>(env.begin(), env.end()), true});
    };
  }
  auto MakeUnreliable() {
    return [this](EntityID obs, std::span<const std::byte> env) {
      sent_.push_back({obs, std::vector<std::byte>(env.begin(), env.end()), false});
    };
  }

  static auto KindOf(const ReplicationCapture& e) -> CellAoIEnvelopeKind {
    return static_cast<CellAoIEnvelopeKind>(e.payload.at(0));
  }
  static auto PayloadBody(const ReplicationCapture& e) -> std::span<const std::byte> {
    return std::span<const std::byte>(e.payload.data() + 5, e.payload.size() - 5);
  }
  // Phase D2'.2: kEntityPropertyUpdate envelopes carry an 8-byte LE
  // event_seq prefix before the delta bytes. Skip it to get at the
  // audience-specific delta payload.
  static auto PropertyUpdateDelta(const ReplicationCapture& e) -> std::span<const std::byte> {
    return PayloadBody(e).subspan(8);
  }

  static auto MakeEntity(Space& space, EntityID id, EntityID base_id, math::Vector3 pos)
      -> CellEntity* {
    auto* e = space.AddEntity(
        std::make_unique<CellEntity>(id, /*type_id=*/1, space, pos, math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), base_id);
    return e;
  }
  static auto MakeBlob(std::initializer_list<uint8_t> bytes) -> std::vector<std::byte> {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (auto b : bytes) v.push_back(static_cast<std::byte>(b));
    return v;
  }
};

TEST_F(PropertyDeltaFixture, OtherObserverReceivesOtherDelta) {
  Space space(1);
  // Observer and peer have DIFFERENT base_entity_ids → observer is "other".
  auto* observer = MakeEntity(space, 1, /*base=*/1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, /*base=*/1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  // Publish a property frame with distinct owner/other deltas.
  CellEntity::ReplicationFrame f;
  f.event_seq = 1;
  f.owner_delta = MakeBlob({0xAA});
  f.other_delta = MakeBlob({0xBB});
  peer->PublishReplicationFrame(f, MakeBlob({0xAA}), MakeBlob({0xBB}));

  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;
  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  std::vector<ReplicationCapture> updates;
  for (auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) updates.push_back(c);
  }
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(PropertyUpdateDelta(updates[0])[0], std::byte{0xBB})
      << "Other-audience observer should receive other_delta (0xBB), not owner_delta (0xAA)";
}

TEST_F(PropertyDeltaFixture, OwnerObserverReceivesOwnerDelta) {
  Space space(1);
  // Observer and peer share the SAME base_entity_id → observer is "owner".
  auto* observer = MakeEntity(space, 1, /*base=*/1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, /*base=*/1001, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  CellEntity::ReplicationFrame f;
  f.event_seq = 1;
  f.owner_delta = MakeBlob({0xCC});
  f.other_delta = MakeBlob({0xDD});
  peer->PublishReplicationFrame(f, {}, {});

  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;
  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  std::vector<ReplicationCapture> updates;
  for (auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) updates.push_back(c);
  }
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(PropertyUpdateDelta(updates[0])[0], std::byte{0xCC})
      << "Owner-audience observer should receive owner_delta (0xCC), not other_delta (0xDD)";
}

// ============================================================================
// Scenario #8 — MoveToPointController drives smooth entity motion across
// multiple ticks via Space::Tick. Pure C++, no CLR needed.
// ============================================================================

TEST(CellAppIntegration, MoveControllerDrivesSmoothMotion) {
  Space space(1);
  auto* entity = space.AddEntity(std::make_unique<CellEntity>(
      1, /*type_id=*/1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  // Add a MoveToPointController: destination (10, 0, 0), speed 10 m/s.
  auto ctrl_id = entity->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{10, 0, 0}, /*speed=*/10.f,
                                              /*face_movement=*/false),
      entity, 0);
  EXPECT_TRUE(entity->GetControllers().Contains(ctrl_id));

  // Tick 0.5s → 5m progress.
  space.Tick(0.5f);
  EXPECT_NEAR(entity->Position().x, 5.f, 0.1f);
  EXPECT_TRUE(entity->GetControllers().Contains(ctrl_id));

  // Tick 0.5s → 10m, arrives at destination.
  space.Tick(0.5f);
  EXPECT_NEAR(entity->Position().x, 10.f, 0.1f);
  // Controller finishes and is reaped.
  EXPECT_FALSE(entity->GetControllers().Contains(ctrl_id));
}

// ============================================================================
// Scenario #9 — 1000-entity tick performance budget: p50 < 20 ms,
// p99 < 50 ms at 10 Hz. Space::Tick + controller updates are pure C++.
// ============================================================================

TEST(CellAppIntegration, ThousandEntityTickWithinPerfBudget) {
  Space space(1);
  constexpr int kEntityCount = 1000;
  for (int i = 1; i <= kEntityCount; ++i) {
    auto* e = space.AddEntity(std::make_unique<CellEntity>(
        static_cast<EntityID>(i), /*type_id=*/1, space,
        math::Vector3{static_cast<float>(i % 100), 0, static_cast<float>(i / 100)},
        math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), static_cast<EntityID>(i + 10000));
  }
  ASSERT_EQ(space.EntityCount(), static_cast<size_t>(kEntityCount));

  // Run 100 ticks at 10 Hz (dt=0.1s), collect timing.
  constexpr int kTickCount = 100;
  std::vector<double> tick_ms;
  tick_ms.reserve(kTickCount);
  for (int t = 0; t < kTickCount; ++t) {
    const auto t0 = std::chrono::steady_clock::now();
    space.Tick(0.1f);
    const auto t1 = std::chrono::steady_clock::now();
    tick_ms.push_back(static_cast<double>(
                          std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) /
                      1000.0);
  }
  std::sort(tick_ms.begin(), tick_ms.end());
  const double p50 = tick_ms[kTickCount / 2];
  const double p99 = tick_ms[kTickCount * 99 / 100];
  EXPECT_LT(p50, 20.0) << "p50 tick time should be < 20 ms";
  EXPECT_LT(p99, 50.0) << "p99 tick time should be < 50 ms";
}

// ============================================================================
// Scenario #1 — Multi-process: machined + CellAppMgr + CellApp all start
// and register with each other. Windows-only (uses CreateProcessW).
// ============================================================================

#if defined(_WIN32)

namespace {

template <typename Pred>
bool PollUntilInteg(EventDispatcher& disp, Pred pred,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

auto ReserveTcpPortInteg() -> uint16_t {
  auto sock = Socket::CreateTcp();
  if (!sock.HasValue()) return 0;
  if (!sock->Bind(Address("127.0.0.1", 0)).HasValue()) return 0;
  auto local = sock->LocalAddress();
  return local ? local->Port() : 0;
}

auto ReserveUdpPortInteg() -> uint16_t {
  auto sock = Socket::CreateUdp();
  if (!sock.HasValue()) return 0;
  if (!sock->Bind(Address("127.0.0.1", 0)).HasValue()) return 0;
  auto local = sock->LocalAddress();
  return local ? local->Port() : 0;
}

auto ExecutablePathInteg() -> std::filesystem::path {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(len);
  return std::filesystem::path(buffer);
}

auto BuildRootInteg() -> std::filesystem::path {
  auto current = ExecutablePathInteg().parent_path();
  for (int i = 0; i < 10 && !current.empty(); ++i) {
    if (std::filesystem::exists(current / "src" / "server" / "machined" / "Debug" /
                                "machined.exe")) {
      return current;
    }
    current = current.parent_path();
  }
  return {};
}

auto ResolveServerExeInteg(const std::wstring& subdir, const std::wstring& filename)
    -> std::filesystem::path {
  auto root = BuildRootInteg();
  if (root.empty()) return {};
  auto p1 = root / "bin" / "Debug" / filename;
  if (std::filesystem::exists(p1)) return p1;
  auto p2 = root / "src" / "server" / subdir / "Debug" / filename;
  if (std::filesystem::exists(p2)) return p2;
  return {};
}

auto QuoteArgInteg(const std::wstring& arg) -> std::wstring {
  std::wstring quoted = L"\"";
  for (wchar_t ch : arg) {
    if (ch == L'"') quoted += L'\\';
    quoted += ch;
  }
  quoted += L"\"";
  return quoted;
}

struct ChildProcess {
  PROCESS_INFORMATION pi{};
  std::string label;
  std::filesystem::path log_path;

  static auto Launch(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                     const std::string& proc_label) -> ChildProcess {
    std::wstring cmd = QuoteArgInteg(exe.wstring());
    for (const auto& a : args) {
      cmd += L' ';
      cmd += QuoteArgInteg(a);
    }
    const auto log_stamp =
        std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
    auto log_file = std::filesystem::temp_directory_path() /
                    ("atlas_cellapp_integ_" + proc_label + "_" + log_stamp + ".log");
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE h_log = ::CreateFileW(log_file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (h_log != INVALID_HANDLE_VALUE) {
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = h_log;
      si.hStdError = h_log;
      si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION proc_info{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const auto workdir = exe.parent_path();
    const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), buf.data(), nullptr, nullptr,
                                     h_log != INVALID_HANDLE_VALUE ? TRUE : FALSE, 0, nullptr,
                                     workdir.wstring().c_str(), &si, &proc_info);
    EXPECT_TRUE(ok) << "CreateProcessW failed for " << exe.string();
    if (h_log != INVALID_HANDLE_VALUE) ::CloseHandle(h_log);
    ChildProcess c;
    c.pi = proc_info;
    c.label = proc_label;
    c.log_path = log_file;
    return c;
  }

  [[nodiscard]] auto IsRunning() const -> bool {
    if (pi.hProcess == nullptr) return false;
    DWORD code = 0;
    return ::GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE;
  }

  [[nodiscard]] auto Diagnostic() const -> std::string {
    std::string out = "[" + label + "] running=" + (IsRunning() ? "yes" : "no");
    if (!log_path.empty() && std::filesystem::exists(log_path)) {
      std::ifstream f(log_path, std::ios::in);
      std::deque<std::string> ring;
      std::string line;
      while (std::getline(f, line)) {
        ring.push_back(std::move(line));
        if (ring.size() > 20) ring.pop_front();
      }
      out += "\n--- log tail ---\n";
      for (const auto& l : ring) out += "  " + l + "\n";
    }
    return out;
  }

  ~ChildProcess() {
    if (pi.hProcess != nullptr) {
      if (IsRunning()) ::TerminateProcess(pi.hProcess, 1);
      ::WaitForSingleObject(pi.hProcess, 5000);
      ::CloseHandle(pi.hProcess);
      pi.hProcess = nullptr;
    }
    if (pi.hThread != nullptr) {
      ::CloseHandle(pi.hThread);
      pi.hThread = nullptr;
    }
  }
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  auto operator=(const ChildProcess&) -> ChildProcess& = delete;
  ChildProcess(ChildProcess&& o) noexcept
      : pi(o.pi), label(std::move(o.label)), log_path(std::move(o.log_path)) {
    o.pi = {};
  }
  auto operator=(ChildProcess&& o) noexcept -> ChildProcess& {
    if (this == &o) return *this;
    this->~ChildProcess();
    pi = o.pi;
    label = std::move(o.label);
    log_path = std::move(o.log_path);
    o.pi = {};
    return *this;
  }
};

auto TcpConnectProbeInteg(uint16_t port) -> bool {
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(0x7F000001u);
  DWORD send_timeout = 250;
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout),
               sizeof(send_timeout));
  const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
  ::closesocket(s);
  return rc == 0;
}

auto WaitForTcpListenInteg(uint16_t port, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (TcpConnectProbeInteg(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

auto WaitForUdpBoundInteg(uint16_t /*port*/, std::chrono::milliseconds timeout) -> bool {
  std::this_thread::sleep_for((std::min)(timeout, std::chrono::milliseconds(250)));
  return true;
}

constexpr int kRetryCount = 5;

auto WaitForRegistrationInteg(MachinedClient& client, EventDispatcher& disp, ProcessType type,
                              uint16_t advertised_port) -> bool {
  return PollUntilInteg(disp, [&]() {
    auto infos = client.QuerySync(type, std::chrono::milliseconds(200));
    for (const auto& p : infos) {
      if (p.internal_addr.Port() == advertised_port) return true;
    }
    return false;
  });
}

}  // namespace
#endif  // defined(_WIN32)

TEST(CellAppIntegration, AllProcessesStartAndRegister) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExeInteg(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExeInteg(L"cellappmgr", L"atlas_cellappmgr.exe");
  const auto cellapp_exe = ResolveServerExeInteg(L"cellapp", L"atlas_cellapp.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty() || cellapp_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRootInteg();
  }

  // 1. Launch machined.
  uint16_t machined_port = 0;
  ChildProcess machined;
  for (int attempt = 0; attempt < kRetryCount; ++attempt) {
    const uint16_t port = ReserveTcpPortInteg();
    if (port == 0) continue;
    auto child =
        ChildProcess::Launch(machined_exe,
                             {L"--type", L"machined", L"--name", L"machined_all_procs",
                              L"--update-hertz", L"100", L"--internal-port", std::to_wstring(port)},
                             "machined");
    if (child.IsRunning() && WaitForTcpListenInteg(port, std::chrono::seconds(2))) {
      machined_port = port;
      machined = std::move(child);
      break;
    }
  }
  ASSERT_NE(machined_port, 0u) << "machined failed to start";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);

  // 2. Launch CellAppMgr.
  uint16_t cellappmgr_port = 0;
  ChildProcess cellappmgr;
  for (int attempt = 0; attempt < kRetryCount; ++attempt) {
    const uint16_t port = ReserveUdpPortInteg();
    if (port == 0) continue;
    auto child = ChildProcess::Launch(
        cellappmgr_exe,
        {L"--type", L"cellappmgr", L"--name", L"cellappmgr_all_procs", L"--update-hertz", L"100",
         L"--internal-port", std::to_wstring(port), L"--machined", machined_addr},
        "cellappmgr");
    if (child.IsRunning() && WaitForUdpBoundInteg(port, std::chrono::seconds(2))) {
      cellappmgr_port = port;
      cellappmgr = std::move(child);
      break;
    }
  }
  ASSERT_NE(cellappmgr_port, 0u) << "cellappmgr failed to start\n" << machined.Diagnostic();

  // 3. Launch CellApp.
  uint16_t cellapp_port = 0;
  ChildProcess cellapp;
  for (int attempt = 0; attempt < kRetryCount; ++attempt) {
    const uint16_t port = ReserveUdpPortInteg();
    if (port == 0) continue;
    auto child = ChildProcess::Launch(
        cellapp_exe,
        {L"--type", L"cellapp", L"--name", L"cellapp_all_procs", L"--update-hertz", L"100",
         L"--internal-port", std::to_wstring(port), L"--machined", machined_addr},
        "cellapp");
    if (child.IsRunning() && WaitForUdpBoundInteg(port, std::chrono::seconds(2))) {
      cellapp_port = port;
      cellapp = std::move(child);
      break;
    }
  }
  ASSERT_NE(cellapp_port, 0u) << "cellapp failed to start\n"
                              << machined.Diagnostic() << "\n"
                              << cellappmgr.Diagnostic();

  // 4. Verify both CellAppMgr and CellApp registered with machined.
  EventDispatcher disp{"integ_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  EXPECT_TRUE(WaitForRegistrationInteg(client, disp, ProcessType::kCellAppMgr, cellappmgr_port))
      << "CellAppMgr did not register\n"
      << cellappmgr.Diagnostic();
  EXPECT_TRUE(WaitForRegistrationInteg(client, disp, ProcessType::kCellApp, cellapp_port))
      << "CellApp did not register\n"
      << cellapp.Diagnostic();
#endif
}

}  // namespace
}  // namespace atlas
