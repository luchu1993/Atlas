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

#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp/intercell_messages.h"
#include "cellapp_messages.h"
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
  }

  auto StartServer() -> Address {
    auto r = network.StartRudpServer(Address("127.0.0.1", 0));
    EXPECT_TRUE(r.HasValue()) << (r.HasValue() ? "" : r.Error().Message());
    return network.RudpAddress();
  }
};

// Pump both dispatchers in round-robin until `pred` holds or timeout.
// FlushDirtySendChannels drains kBatched messages out of the per-channel
// deferred bundle - without it, CreateGhost/GhostDelta/GhostPositionUpdate
// (all kBatched) never leave the sender.
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

    real = A.app.FindEntityByBaseId(kBaseId);
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

// =============================================================================
// 1. CreateGhost over real RUDP materialises a Ghost on the peer.
// =============================================================================

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

// =============================================================================
// 2. GhostPositionUpdate advances the peer Ghost's volatile state.
// =============================================================================

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

// =============================================================================
// 3. GhostDelta advances the peer Ghost's replication seq.
//
// Seed the Ghost with event_seq=5 so the first delta at seq=6 is a valid
// in-order advance (GhostApplyDelta drops stale / non-advancing seqs).
// =============================================================================

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

// =============================================================================
// 4. OffloadEntity over RUDP rehydrates a Real on the peer AND the
//    receiver's OffloadEntityAck round-trips.
//
// We seed A's pending_offloads_ manually (bypassing TickOffloadChecker,
// which would also drive ConvertRealToGhost + send — both tested in
// unit-level tests). The ack-observable assertion is that A's
// pending_offloads_ drains once the ack arrives.
// =============================================================================

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
  EXPECT_EQ(fx.B.app.FindEntityByBaseId(offload.entity_id), rehydrated);

  // OffloadEntityAck round-trip: A's pending entry drains once the ack
  // arrives on the bidirectional RUDP channel.
  ASSERT_TRUE(PumpUntil(fx.A, fx.B, [&] { return fx.A.app.PendingOffloadsForTest().empty(); }))
      << "OffloadEntityAck did not round-trip to sender (pending_offloads not drained)";
}

}  // namespace atlas
