// Offload traversal test — natural movement across a BSP boundary.
//
// test_distributed_space drives OffloadEntity manually over RUDP.
// This test closes the gap by exercising the CellApp::TickOffloadChecker
// pump path: configure a BSP tree with a split at x=0 (host A owns x<0,
// host B owns x>=0), park a Real on A at x=-5 inside a local Cell,
// then move it to x=+5 and pump TickOffloadChecker. The checker is
// expected to:
//
//   1. Query the BSP for the target cell at the new position,
//   2. Build an OffloadEntity, GhostSetNextReal, and ship them to B,
//   3. ConvertRealToGhost on A so the peer becomes a back-channel,
//   4. Drop the entity from A's local Cell,
//   5. Receive OffloadEntityAck and drain the pending_offloads_ entry.
//
// B's side:
//   6. Auto-creates the Space on CreateGhost-less arrival,
//   7. Rehydrates the Real (no Ghost existed pre-offload → fresh Real),
//   8. Sends OffloadEntityAck back.

#include <chrono>
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
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "space.h"

namespace atlas {
namespace {

struct Host {
  EventDispatcher dispatcher;
  NetworkInterface network;
  CellApp app;

  explicit Host(std::string name)
      : dispatcher(std::move(name)), network(dispatcher), app(dispatcher, network) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    auto& t = network.InterfaceTable();
    (void)t.RegisterTypedHandler<cellapp::OffloadEntity>(
        [this](const Address& src, Channel* ch, const cellapp::OffloadEntity& m) {
          app.OnOffloadEntity(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::OffloadEntityAck>(
        [this](const Address& src, Channel* ch, const cellapp::OffloadEntityAck& m) {
          app.OnOffloadEntityAck(src, ch, m);
        });
    (void)t.RegisterTypedHandler<cellapp::GhostSetNextReal>(
        [this](const Address& src, Channel* ch, const cellapp::GhostSetNextReal& m) {
          app.OnGhostSetNextReal(src, ch, m);
        });
  }

  auto StartServer() -> Address {
    auto r = network.StartRudpServer(Address("127.0.0.1", 0));
    EXPECT_TRUE(r.HasValue());
    return network.RudpAddress();
  }
};

auto PumpUntil(Host& a, Host& b, const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a.dispatcher.ProcessOnce();
    b.dispatcher.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

// Build a BSP with one split at x=0: cell 1 (for `left`) owns x<0,
// cell 2 (for `right`) owns x>=0. Both halves span the full Z axis.
auto BuildTwoCellBsp(const Address& left, const Address& right) -> BSPTree {
  BSPTree tree;
  CellInfo c1{/*cell_id=*/1, left, /*bounds=*/{}, /*load=*/0.f, /*entity_count=*/0};
  tree.InitSingleCell(c1);
  CellInfo c2{/*cell_id=*/2, right, /*bounds=*/{}, 0.f, 0};
  auto r = tree.Split(/*existing=*/1, BSPAxis::kX, /*position=*/0.f, c2);
  EXPECT_TRUE(r.HasValue());
  return tree;
}

TEST(OffloadTraversal, EntityCrossesBspSplit_PumpsOffload) {
  Host A("offload_traversal_a");
  Host B("offload_traversal_b");
  auto addr_a = A.StartServer();
  auto addr_b = B.StartServer();
  ASSERT_NE(addr_a.Port(), 0u);
  ASSERT_NE(addr_b.Port(), 0u);

  // Open the A→B channel (real RUDP) and seed A's peer registry so
  // TickOffloadChecker's FindPeerChannel(B) resolves.
  auto ch_a2b = A.network.ConnectRudp(addr_b);
  ASSERT_TRUE(ch_a2b.HasValue());
  A.app.PeerRegistryForTest().InsertForTest(addr_b, *ch_a2b);
  auto ch_b2a = B.network.ConnectRudp(addr_a);
  ASSERT_TRUE(ch_b2a.HasValue());
  B.app.PeerRegistryForTest().InsertForTest(addr_a, *ch_b2a);

  // Configure the space on A: BSP with x=0 split, local Cell 1 for the
  // "A owns" half (x<0), containing the Real we'll offload.
  const SpaceID kSpaceId = 42;
  cellapp::CreateSpace cs{kSpaceId};
  A.app.OnCreateSpace({}, nullptr, cs);
  auto* space_a = A.app.FindSpace(kSpaceId);
  ASSERT_NE(space_a, nullptr);
  space_a->SetBspTree(BuildTwoCellBsp(addr_a, addr_b));
  auto* cell_a =
      space_a->AddLocalCell(std::make_unique<Cell>(*space_a, /*cell_id=*/1, CellBounds{}));
  ASSERT_NE(cell_a, nullptr);

  // Spawn a Real entity on A at x=-5 (inside cell 1) and register it
  // in the local Cell so TickOffloadChecker can iterate real entities.
  cellapp::CreateCellEntity cce;
  cce.entity_id = 100;
  cce.type_id = 1;
  cce.space_id = kSpaceId;
  cce.position = {-5.f, 0.f, 0.f};
  cce.direction = {1.f, 0.f, 0.f};
  A.app.OnCreateCellEntity({}, nullptr, cce);
  auto* real = A.app.FindEntityByBaseId(100);
  ASSERT_NE(real, nullptr);
  ASSERT_TRUE(real->IsReal());

  // Move it across the split. TickOffloadChecker will query the BSP
  // using the new position, see it lands on cell 2 (B), and emit the
  // offload.
  real->SetPosition({+5.f, 0.f, 0.f});

  A.app.TickOffloadChecker();

  // Wait for the round-trip:
  //   - B rehydrates a Real at base_id=100 (OnOffloadEntity fires),
  //   - A's pending_offloads_ drains when OffloadEntityAck arrives.
  const auto real_cell_id = real->Id();
  ASSERT_TRUE(PumpUntil(A, B, [&] {
    auto* on_b = B.app.FindEntity(real_cell_id);
    return on_b != nullptr && on_b->IsReal();
  })) << "B never rehydrated the Real from the pump-driven Offload";

  ASSERT_TRUE(PumpUntil(A, B, [&] { return A.app.PendingOffloadsForTest().empty(); }))
      << "A's pending_offloads_ never drained — Ack round-trip stuck";

  // A's copy should now be a Ghost pointing at B.
  auto* on_a = A.app.FindEntity(real_cell_id);
  ASSERT_NE(on_a, nullptr);
  EXPECT_TRUE(on_a->IsGhost()) << "A's entity should have flipped to Ghost after Offload";

  auto* on_b = B.app.FindEntity(real_cell_id);
  ASSERT_NE(on_b, nullptr);
  EXPECT_TRUE(on_b->IsReal());
  EXPECT_FLOAT_EQ(on_b->Position().x, 5.f);
  EXPECT_EQ(on_b->Id(), 100u);
}

// Entity that stays inside its original cell must not trigger an
// Offload: TickOffloadChecker must ask the BSP, see same cell, and
// skip. This pins the "no Offload when staying put" invariant that
// would otherwise thrash on every move.
TEST(OffloadTraversal, EntityStaysInOwnCell_NoOffload) {
  Host A("offload_no_traversal_a");
  Host B("offload_no_traversal_b");
  auto addr_a = A.StartServer();
  auto addr_b = B.StartServer();

  auto ch_a2b = A.network.ConnectRudp(addr_b);
  ASSERT_TRUE(ch_a2b.HasValue());
  A.app.PeerRegistryForTest().InsertForTest(addr_b, *ch_a2b);

  const SpaceID kSpaceId = 7;
  A.app.OnCreateSpace({}, nullptr, cellapp::CreateSpace{kSpaceId});
  auto* space_a = A.app.FindSpace(kSpaceId);
  space_a->SetBspTree(BuildTwoCellBsp(addr_a, addr_b));
  auto* cell_a =
      space_a->AddLocalCell(std::make_unique<Cell>(*space_a, /*cell_id=*/1, CellBounds{}));
  (void)cell_a;

  cellapp::CreateCellEntity cce;
  cce.entity_id = 101;
  cce.type_id = 1;
  cce.space_id = kSpaceId;
  cce.position = {-5.f, 0.f, 0.f};
  cce.direction = {1.f, 0.f, 0.f};
  A.app.OnCreateCellEntity({}, nullptr, cce);
  auto* real = A.app.FindEntityByBaseId(101);
  ASSERT_TRUE(real->IsReal());

  // Move the entity but keep it inside cell 1 (x stays < 0).
  real->SetPosition({-8.f, 0.f, 3.f});
  A.app.TickOffloadChecker();

  EXPECT_TRUE(A.app.PendingOffloadsForTest().empty())
      << "Move within own cell must not enqueue a pending Offload";
  EXPECT_TRUE(real->IsReal()) << "Entity must still be Real on A";
}

}  // namespace
}  // namespace atlas
