// End-to-end Real/Ghost/Offload over real RUDP.
//
// Two CellApp instances share one test thread, each bound to its own
// EventDispatcher + NetworkInterface on an ephemeral 127.0.0.1 UDP port.
// We register the inter-CellApp handlers directly on each side's
// InterfaceTable (bypassing CellApp::Init — which would try to stand up
// the CLR, machined subscription, and a CellAppMgr connection), then
// drive the canonical Real → Ghost → Offload → Real-on-peer handshake
// by pushing the wire messages through real sockets.
//
// Coverage intent:
//   1. CreateGhost over RUDP materialises a Ghost on the peer CellApp.
//   2. GhostPositionUpdate advances the peer Ghost's volatile state.
//   3. GhostDelta advances the peer Ghost's replication seq + baseline.
//   4. OffloadEntity over RUDP rehydrates a Real on the peer AND the
//      receiver's OffloadEntityAck round-trips to the sender (observed
//      via pending_offloads_ draining).
//
// Non-wire machinery (TickGhostPump / TickOffloadChecker / BSP-driven
// triggers / C# persistence) is exercised elsewhere — test_real_ghost,
// test_ghost_maintainer, test_offload_checker, test_cellapp_handlers.
// This test focuses on the wire handshake.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp/intercell_messages.h"
#include "cellapp_messages.h"
#include "cellappmgr/bsp_tree.h"
#include "entitydef/entity_def_registry.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/socket.h"
#include "space.h"

namespace atlas {
namespace {

// ----------------------------------------------------------------------------
// Host wrapper: one EventDispatcher + NetworkInterface + CellApp, with
// the intercell handlers wired directly onto the InterfaceTable. No
// Init() call — Init would attempt machined / CellAppMgr bring-up we
// don't want in a handler-level integration test.
// ----------------------------------------------------------------------------

struct CellAppHost {
  EventDispatcher dispatcher;
  NetworkInterface network;
  CellApp app;

  explicit CellAppHost(std::string name)
      : dispatcher(std::move(name)), network(dispatcher), app(dispatcher, network) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    auto& t = network.InterfaceTable();

    (void)t.RegisterTypedHandler<cellapp::CreateGhost>(
        [this](const Address& src, Channel* ch, const cellapp::CreateGhost& m) {
          app.OnCreateGhost(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::DeleteGhost>(
        [this](const Address& src, Channel* ch, const cellapp::DeleteGhost& m) {
          app.OnDeleteGhost(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::GhostPositionUpdate>(
        [this](const Address& src, Channel* ch, const cellapp::GhostPositionUpdate& m) {
          app.OnGhostPositionUpdate(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::GhostDelta>(
        [this](const Address& src, Channel* ch, const cellapp::GhostDelta& m) {
          app.OnGhostDelta(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::GhostSnapshotRefresh>(
        [this](const Address& src, Channel* ch, const cellapp::GhostSnapshotRefresh& m) {
          app.OnGhostSnapshotRefresh(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::OffloadEntity>(
        [this](const Address& src, Channel* ch, const cellapp::OffloadEntity& m) {
          app.OnOffloadEntity(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::OffloadEntityAck>(
        [this](const Address& src, Channel* ch, const cellapp::OffloadEntityAck& m) {
          app.OnOffloadEntityAck(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::SpaceDataUpdate>(
        [this](const Address& src, Channel* ch, const cellapp::SpaceDataUpdate& m) {
          app.OnSpaceDataUpdate(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::SpaceDataDelete>(
        [this](const Address& src, Channel* ch, const cellapp::SpaceDataDelete& m) {
          app.OnSpaceDataDelete(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::SpaceDataSnapshotRequest>(
        [this](const Address& src, Channel* ch,
               const cellapp::SpaceDataSnapshotRequest& m) {
          app.OnSpaceDataSnapshotRequest(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::SpaceDataSnapshot>(
        [this](const Address& src, Channel* ch, const cellapp::SpaceDataSnapshot& m) {
          app.OnSpaceDataSnapshot(src, ch, m);
        });
  }

  auto StartServer() -> Address {
    auto r = network.StartRudpServer(Address("127.0.0.1", 0));
    EXPECT_TRUE(r.HasValue()) << (r.HasValue() ? "" : r.Error().Message());
    return network.RudpAddress();
  }
};

// Pump both dispatchers and kBatched sends until `pred` holds or timeout.
auto PumpUntil(CellAppHost& a, CellAppHost& b, const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a.network.FlushDirtySendChannels();
    b.network.FlushDirtySendChannels();
    a.dispatcher.ProcessOnce();
    b.dispatcher.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

// ----------------------------------------------------------------------------
// Shared fixture: two CellApp hosts wired via a real RUDP channel, with a
// fresh Real CellEntity already placed on the A side.
// ----------------------------------------------------------------------------

struct RealGhostFixture {
  CellAppHost A{"c11_cellapp_a"};
  CellAppHost B{"c11_cellapp_b"};
  Address addr_a;
  Address addr_b;
  ReliableUdpChannel* ch_a_to_b{nullptr};
  CellEntity* real{nullptr};
  static constexpr SpaceID kSpaceId = 1;
  static constexpr EntityID kBaseId = 100;

  RealGhostFixture() {
    EntityDefRegistry::Instance().clear();
    addr_a = A.StartServer();
    addr_b = B.StartServer();

    auto r = A.network.ConnectRudp(addr_b);
    EXPECT_TRUE(r.HasValue()) << (r.HasValue() ? "" : r.Error().Message());
    if (r.HasValue()) ch_a_to_b = *r;

    cellapp::CreateSpace cs;
    cs.space_id = kSpaceId;
    A.app.OnCreateSpace({}, nullptr, cs);
    B.app.OnCreateSpace({}, nullptr, cs);

    cellapp::CreateCellEntity cce;
    cce.entity_id = kBaseId;
    cce.type_id = 1;
    cce.space_id = kSpaceId;
    cce.position = {50.f, 0.f, 50.f};
    cce.direction = {1.f, 0.f, 0.f};
    A.app.OnCreateCellEntity({}, nullptr, cce);

    real = A.app.FindRealEntity(kBaseId);
    EXPECT_NE(real, nullptr);
    if (real != nullptr) {
      EXPECT_TRUE(real->IsReal());
    }
  }

  ~RealGhostFixture() { EntityDefRegistry::Instance().clear(); }

  auto MakeCreateGhost(uint64_t event_seq = 0, uint64_t volatile_seq = 0) -> cellapp::CreateGhost {
    cellapp::CreateGhost cg;
    cg.entity_id = real->Id();
    cg.type_id = 1;
    cg.space_id = kSpaceId;
    cg.position = real->Position();
    cg.direction = real->Direction();
    cg.on_ground = real->OnGround();
    cg.real_cellapp_addr = addr_a;
    cg.base_addr = Address(0, 0);
    cg.entity_id = real->Id();
    cg.event_seq = event_seq;
    cg.volatile_seq = volatile_seq;
    return cg;
  }
};

}  // namespace

TEST(DistributedSpaceOverRudp, CreateGhost_InstantiatesGhostOnPeer) {
  RealGhostFixture fx;
  ASSERT_NE(fx.addr_a.Port(), 0u);
  ASSERT_NE(fx.addr_b.Port(), 0u);
  ASSERT_NE(fx.real, nullptr);
  ASSERT_NE(fx.ch_a_to_b, nullptr);

  auto cg = fx.MakeCreateGhost();
  ASSERT_TRUE(fx.ch_a_to_b->SendMessage(cg).HasValue());

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] { return fx.B.app.FindEntity(cg.entity_id) != nullptr; }))
      << "Ghost never materialised on peer CellApp";

  auto* ghost = fx.B.app.FindEntity(cg.entity_id);
  ASSERT_NE(ghost, nullptr);
  EXPECT_TRUE(ghost->IsGhost());
  EXPECT_FLOAT_EQ(ghost->Position().x, cg.position.x);
  EXPECT_FLOAT_EQ(ghost->Position().z, cg.position.z);
  EXPECT_EQ(ghost->Id(), cg.entity_id);
}

TEST(DistributedSpaceOverRudp, GhostPositionUpdate_AdvancesPeerGhost) {
  RealGhostFixture fx;
  ASSERT_NE(fx.ch_a_to_b, nullptr);

  auto cg = fx.MakeCreateGhost();
  ASSERT_TRUE(fx.ch_a_to_b->SendMessage(cg).HasValue());
  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] { return fx.B.app.FindEntity(cg.entity_id) != nullptr; }));

  cellapp::GhostPositionUpdate gpu;
  gpu.entity_id = cg.entity_id;
  gpu.position = {75.f, 0.f, 80.f};
  gpu.direction = {0.f, 0.f, 1.f};
  gpu.on_ground = true;
  gpu.volatile_seq = 1;
  ASSERT_TRUE(fx.ch_a_to_b->SendMessage(gpu).HasValue());

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto* g = fx.B.app.FindEntity(cg.entity_id);
    return g != nullptr && g->Position().x > 74.f && g->Position().z > 79.f;
  })) << "GhostPositionUpdate did not advance peer Ghost";

  auto* g = fx.B.app.FindEntity(cg.entity_id);
  ASSERT_NE(g, nullptr);
  EXPECT_TRUE(g->OnGround());
  EXPECT_FLOAT_EQ(g->Direction().z, 1.f);
}

// Seeds the Ghost with event_seq=5 so the first delta at seq=6 is a
// valid in-order advance (GhostApplyDelta drops non-advancing seqs).
TEST(DistributedSpaceOverRudp, GhostDelta_AdvancesPeerReplicationSeq) {
  RealGhostFixture fx;
  ASSERT_NE(fx.ch_a_to_b, nullptr);

  auto cg = fx.MakeCreateGhost(/*event_seq=*/5);
  cg.other_snapshot = {std::byte{0x11}, std::byte{0x22}};
  ASSERT_TRUE(fx.ch_a_to_b->SendMessage(cg).HasValue());

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto* g = fx.B.app.FindEntity(cg.entity_id);
    return g != nullptr && g->GetReplicationState() != nullptr &&
           g->GetReplicationState()->latest_event_seq == 5u;
  })) << "Ghost did not pick up seeded event_seq from CreateGhost";

  cellapp::GhostDelta gd;
  gd.entity_id = cg.entity_id;
  gd.event_seq = 6;
  gd.other_delta = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  ASSERT_TRUE(fx.ch_a_to_b->SendMessage(gd).HasValue());

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto* g = fx.B.app.FindEntity(cg.entity_id);
    return g != nullptr && g->GetReplicationState() != nullptr &&
           g->GetReplicationState()->latest_event_seq == 6u;
  })) << "GhostDelta did not advance Ghost replication seq on peer";
}

// Seeds A's pending_offloads_ manually (bypassing TickOffloadChecker,
// covered by unit tests). Ack drains the pending entry on success.
TEST(DistributedSpaceOverRudp, OffloadEntity_RehydratesPeerRealAndAcks) {
  RealGhostFixture fx;
  ASSERT_NE(fx.ch_a_to_b, nullptr);

  auto offload = fx.A.app.BuildOffloadMessage(*fx.real);
  ASSERT_EQ(offload.entity_id, fx.real->Id());
  ASSERT_EQ(offload.space_id, fx.real->GetSpace().Id());
  ASSERT_EQ(offload.entity_id, fx.real->Id());

  // Install a pending entry so the receiver's ack has something to
  // resolve on A. Production path inserts this in TickOffloadChecker.
  auto& pending = fx.A.app.PendingOffloadsForTest();
  CellApp::PendingOffload p;
  p.target_addr = fx.addr_b;
  p.sent_at = Clock::now();
  p.space_id = offload.space_id;
  pending[offload.entity_id] = std::move(p);
  ASSERT_EQ(pending.size(), 1u);

  ASSERT_TRUE(fx.ch_a_to_b->SendMessage(offload).HasValue());

  // B rehydrates a Real at the offloaded id.
  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto* e = fx.B.app.FindEntity(offload.entity_id);
    return e != nullptr && e->IsReal();
  })) << "Peer did not rehydrate Real from OffloadEntity";

  auto* rehydrated = fx.B.app.FindEntity(offload.entity_id);
  ASSERT_NE(rehydrated, nullptr);
  EXPECT_TRUE(rehydrated->IsReal());
  EXPECT_EQ(rehydrated->Id(), offload.entity_id);
  EXPECT_FLOAT_EQ(rehydrated->Position().x, offload.position.x);
  EXPECT_FLOAT_EQ(rehydrated->Position().z, offload.position.z);
  EXPECT_EQ(fx.B.app.FindRealEntity(offload.entity_id), rehydrated);

  // OffloadEntityAck round-trip: A's pending entry drains once the ack
  // arrives on the bidirectional RUDP channel.
  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] { return fx.A.app.PendingOffloadsForTest().empty(); }))
      << "OffloadEntityAck did not round-trip to sender (pending_offloads not drained)";
}

namespace {

// Two-cellapp BSP where A holds the primary leaf (= owner), B holds the
// split-right leaf (= ghost). Cross-RUDP peer channels pre-registered.
struct SpaceDataFixture {
  CellAppHost A{"sd_a"};
  CellAppHost B{"sd_b"};
  Address addr_a;
  Address addr_b;
  ReliableUdpChannel* ch_a_to_b{nullptr};
  ReliableUdpChannel* ch_b_to_a{nullptr};
  static constexpr SpaceID kSpaceId = 7;
  static constexpr cellappmgr::CellID kCellPrimary = 1;
  static constexpr cellappmgr::CellID kCellRight = 2;

  SpaceDataFixture() {
    EntityDefRegistry::Instance().clear();
    addr_a = A.StartServer();
    addr_b = B.StartServer();
    auto r_ab = A.network.ConnectRudp(addr_b);
    auto r_ba = B.network.ConnectRudp(addr_a);
    EXPECT_TRUE(r_ab.HasValue()) << (r_ab.HasValue() ? "" : r_ab.Error().Message());
    EXPECT_TRUE(r_ba.HasValue()) << (r_ba.HasValue() ? "" : r_ba.Error().Message());
    ch_a_to_b = r_ab.HasValue() ? *r_ab : nullptr;
    ch_b_to_a = r_ba.HasValue() ? *r_ba : nullptr;

    cellapp::CreateSpace cs;
    cs.space_id = kSpaceId;
    A.app.OnCreateSpace({}, nullptr, cs);
    B.app.OnCreateSpace({}, nullptr, cs);

    // Both sides receive the same split BSP — owner A also needs to know
    // B holds the right leaf so BroadcastSpaceDataUpdate finds B as a peer.
    Plant(A.app, kCellPrimary, addr_a, /*split_right=*/std::make_optional(addr_b));
    Plant(B.app, kCellRight, addr_a, /*split_right=*/std::make_optional(addr_b));

    A.app.PeerRegistryForTest().InsertForTest(addr_b, ch_a_to_b);
    B.app.PeerRegistryForTest().InsertForTest(addr_a, ch_b_to_a);
  }

  ~SpaceDataFixture() { EntityDefRegistry::Instance().clear(); }

  // Builds a single-leaf or split BSP tree and registers the local cell
  // that this cellapp authoritatively holds.
  static void Plant(CellApp& app, cellappmgr::CellID local_cell_id, const Address& primary_addr,
                    std::optional<Address> split_right_addr) {
    auto* space = app.FindSpace(kSpaceId);
    BSPTree tree;
    CellInfo primary;
    primary.cell_id = kCellPrimary;
    primary.cellapp_addr = primary_addr;
    tree.InitSingleCell(primary);
    if (split_right_addr.has_value()) {
      CellInfo right;
      right.cell_id = kCellRight;
      right.cellapp_addr = *split_right_addr;
      ASSERT_TRUE(tree.Split(kCellPrimary, BSPAxis::kX, 0.f, right).HasValue());
    }
    space->AddLocalCell(std::make_unique<Cell>(*space, local_cell_id, CellBounds{}));
    space->SetBspTree(std::move(tree));
  }
};

}  // namespace

TEST(SpaceDataOverRudp, OwnerWriteMirrorsToGhost) {
  SpaceDataFixture fx;
  ASSERT_TRUE(fx.A.app.FindSpace(fx.kSpaceId)->IsOwner());
  ASSERT_FALSE(fx.B.app.FindSpace(fx.kSpaceId)->IsOwner());

  const uint8_t value[] = {0x01, 0x02, 0x03, 0x04};
  fx.A.app.SetSpaceData(fx.kSpaceId, /*key_id=*/10,
                        std::span<const uint8_t>(value, 4));

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto* v = fx.B.app.FindSpace(fx.kSpaceId)->Data().Get(10);
    return v != nullptr && v->size() == 4;
  })) << "Ghost B never observed SpaceData update from owner A";

  EXPECT_EQ(*fx.B.app.FindSpace(fx.kSpaceId)->Data().Get(10),
            (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
}

TEST(SpaceDataOverRudp, NonOwnerWriteForwardsToOwner) {
  SpaceDataFixture fx;
  const uint8_t value[] = {0xAA, 0xBB};
  fx.B.app.SetSpaceData(fx.kSpaceId, /*key_id=*/20,
                        std::span<const uint8_t>(value, 2));

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto* v = fx.A.app.FindSpace(fx.kSpaceId)->Data().Get(20);
    return v != nullptr && v->size() == 2;
  })) << "Owner A never received forwarded SpaceData write from ghost B";
}

TEST(SpaceDataOverRudp, OwnerDeleteMirrorsToGhost) {
  SpaceDataFixture fx;
  const uint8_t value[] = {0x42};
  fx.A.app.SetSpaceData(fx.kSpaceId, 30, std::span<const uint8_t>(value, 1));
  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    return fx.B.app.FindSpace(fx.kSpaceId)->Data().Contains(30);
  }));

  fx.A.app.RemoveSpaceData(fx.kSpaceId, 30);
  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    return !fx.B.app.FindSpace(fx.kSpaceId)->Data().Contains(30);
  })) << "Ghost B did not observe SpaceData delete from owner A";
}

TEST(SpaceDataOverRudp, SnapshotRequestSeedsLateJoiner) {
  SpaceDataFixture fx;
  const uint8_t a[] = {0x01};
  const uint8_t b[] = {0x02, 0x03};
  fx.A.app.SetSpaceData(fx.kSpaceId, 1, std::span<const uint8_t>(a, 1));
  fx.A.app.SetSpaceData(fx.kSpaceId, 2, std::span<const uint8_t>(b, 2));
  fx.B.app.FindSpace(fx.kSpaceId)->Data().Clear();

  cellapp::SpaceDataSnapshotRequest req;
  req.space_id = fx.kSpaceId;
  ASSERT_TRUE(fx.ch_b_to_a->SendMessage(req).HasValue());

  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] {
    auto& d = fx.B.app.FindSpace(fx.kSpaceId)->Data();
    return d.Size() == 2 && d.Contains(1) && d.Contains(2);
  })) << "Late joiner B did not receive SpaceData snapshot from owner A";

  EXPECT_TRUE(fx.B.app.FindSpace(fx.kSpaceId)->IsDataInitialized());
}

}  // namespace atlas
