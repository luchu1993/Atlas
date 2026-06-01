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
#include "movement_sim/movement_sim.h"
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

auto BuildTwoCellBsp(const Address& left, const Address& right) -> BSPTree {
  BSPTree tree;
  CellInfo c1{/*cell_id=*/1, left, /*bounds=*/{}, /*load=*/0.f, /*entity_count=*/0};
  tree.InitSingleCell(c1);
  CellInfo c2{/*cell_id=*/2, right, /*bounds=*/{}, 0.f, 0};
  auto r = tree.Split(/*existing=*/1, BSPAxis::kX, /*position=*/0.f, c2);
  EXPECT_TRUE(r.HasValue());
  return tree;
}

auto MovementStateAt(float x, uint32_t last_processed_seq) -> movement::MovementState {
  movement::MovementState state;
  state.position = {x, 0.0f, 0.0f};
  state.velocity = {0.0f, 0.0f, 0.0f};
  state.direction = {1.0f, 0.0f, 0.0f};
  state.flags = movement::kMovementFlagGrounded;
  state.last_processed_input_seq = last_processed_seq;
  return state;
}

auto MovementCommandAt(float from_x, float to_x) -> movement::MovementCommand {
  movement::MovementCommand command;
  command.command_id = 77;
  command.start_position = {from_x, 0.0f, 0.0f};
  command.target_position = {to_x, 0.0f, 0.0f};
  command.duration_ms = 900;
  command.elapsed_ms = 300;
  command.curve_id = 4;
  return command;
}

TEST(OffloadTraversal, EntityCrossesBspSplit_PumpsOffload) {
  Host host_a("offload_traversal_a");
  Host host_b("offload_traversal_b");
  auto addr_a = host_a.StartServer();
  auto addr_b = host_b.StartServer();
  ASSERT_NE(addr_a.Port(), 0u);
  ASSERT_NE(addr_b.Port(), 0u);

  auto ch_a2b = host_a.network.ConnectRudp(addr_b);
  ASSERT_TRUE(ch_a2b.HasValue());
  host_a.app.PeerRegistryForTest().InsertForTest(addr_b, *ch_a2b);
  auto ch_b2a = host_b.network.ConnectRudp(addr_a);
  ASSERT_TRUE(ch_b2a.HasValue());
  host_b.app.PeerRegistryForTest().InsertForTest(addr_a, *ch_b2a);

  const SpaceID kSpaceId = 42;
  cellapp::CreateSpace cs{kSpaceId};
  host_a.app.OnCreateSpace({}, nullptr, cs);
  auto* space_a = host_a.app.FindSpace(kSpaceId);
  ASSERT_NE(space_a, nullptr);
  space_a->SetBspTree(BuildTwoCellBsp(addr_a, addr_b));
  auto* cell_a =
      space_a->AddLocalCell(std::make_unique<Cell>(*space_a, /*cell_id=*/1, CellBounds{}));
  ASSERT_NE(cell_a, nullptr);

  cellapp::CreateCellEntity cce;
  cce.entity_id = 100;
  cce.type_id = 1;
  cce.space_id = kSpaceId;
  cce.position = {-5.f, 0.f, 0.f};
  cce.direction = {1.f, 0.f, 0.f};
  host_a.app.OnCreateCellEntity({}, nullptr, cce);
  auto* real = host_a.app.FindRealEntity(100);
  ASSERT_NE(real, nullptr);
  ASSERT_TRUE(real->IsReal());

  // Source ticks chosen low so the rebase on B preserves both samples
  // regardless of B's MovementServerTick at restore time (a fresh test
  // cell typically still sits near tick 0). In production the captured
  // ticks are always ≤ source's current tick, so this invariant holds.
  host_a.app.MovementSystemForTest().position_history().Record(100, 0, MovementStateAt(-5.0f, 7));
  host_a.app.MovementSystemForTest().position_history().Record(100, 1, MovementStateAt(25.0f, 8));
  ASSERT_TRUE(host_a.app.MovementSystemForTest().command_store().Set(100, MovementCommandAt(-5.0f, 25.0f)));
  real->SetPosition({+25.f, 0.f, 0.f});

  host_a.app.TickOffloadChecker();

  const auto real_cell_id = real->Id();
  ASSERT_TRUE(PumpUntil(host_a, host_b, [&] {
    auto* on_b = host_b.app.FindEntity(real_cell_id);
    return on_b != nullptr && on_b->IsReal();
  })) << "B never rehydrated the Real from the pump-driven Offload";

  ASSERT_TRUE(
      PumpUntil(host_a, host_b, [&] { return host_a.app.PendingOffloadsForTest().empty(); }))
      << "A's pending_offloads_ never drained";

  auto* on_a = host_a.app.FindEntity(real_cell_id);
  ASSERT_NE(on_a, nullptr);
  EXPECT_TRUE(on_a->IsGhost()) << "A's entity should have flipped to Ghost after Offload";

  auto* on_b = host_b.app.FindEntity(real_cell_id);
  ASSERT_NE(on_b, nullptr);
  EXPECT_TRUE(on_b->IsReal());
  EXPECT_FLOAT_EQ(on_b->Position().x, 25.f);
  EXPECT_EQ(on_b->Id(), 100u);

  EXPECT_EQ(host_a.app.MovementSystemForTest().position_history().Find(real_cell_id), nullptr);
  // Samples from A get rebased so the latest sits at B's server_tick; in a
  // fresh test where B hasn't ticked, the older sample (rebased to -1)
  // drops to underflow guard, leaving the newest one intact. In production
  // both cells would have advanced enough for the full window to survive.
  const auto* on_b_history =
      host_b.app.MovementSystemForTest().position_history().Find(real_cell_id);
  ASSERT_NE(on_b_history, nullptr);
  ASSERT_GE(on_b_history->size(), 1u);
  const auto& latest_sample = on_b_history->back();
  EXPECT_FLOAT_EQ(latest_sample.state.position.x, 25.0f);
  EXPECT_EQ(latest_sample.state.last_processed_input_seq, 8u);

  EXPECT_EQ(host_a.app.MovementSystemForTest().command_store().Find(real_cell_id), nullptr);
  const auto* command = host_b.app.MovementSystemForTest().command_store().Find(real_cell_id);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command_id, 77u);
  EXPECT_EQ(command->elapsed_ms, 300u);
  EXPECT_FLOAT_EQ(command->target_position.x, 25.0f);
}

TEST(OffloadTraversal, EntityStaysInOwnCell_NoOffload) {
  Host host_a("offload_no_traversal_a");
  Host host_b("offload_no_traversal_b");
  auto addr_a = host_a.StartServer();
  auto addr_b = host_b.StartServer();

  auto ch_a2b = host_a.network.ConnectRudp(addr_b);
  ASSERT_TRUE(ch_a2b.HasValue());
  host_a.app.PeerRegistryForTest().InsertForTest(addr_b, *ch_a2b);

  const SpaceID kSpaceId = 7;
  host_a.app.OnCreateSpace({}, nullptr, cellapp::CreateSpace{kSpaceId});
  auto* space_a = host_a.app.FindSpace(kSpaceId);
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
  host_a.app.OnCreateCellEntity({}, nullptr, cce);
  auto* real = host_a.app.FindRealEntity(101);
  ASSERT_TRUE(real->IsReal());

  real->SetPosition({-8.f, 0.f, 3.f});
  host_a.app.TickOffloadChecker();

  EXPECT_TRUE(host_a.app.PendingOffloadsForTest().empty())
      << "Move within own cell must not enqueue a pending Offload";
  EXPECT_TRUE(real->IsReal()) << "Entity must still be Real on A";
}

// Cross-space teleport: the resolved-host reply kicks an is_teleport Offload
// with geometry_version=0; the destination re-localizes via its own BSP and
// rehydrates the Real in the target space.
TEST(OffloadTraversal, CrossSpaceTeleport_RehydratesInTargetSpace) {
  Host host_a("teleport_traversal_a");
  Host host_b("teleport_traversal_b");
  auto addr_a = host_a.StartServer();
  auto addr_b = host_b.StartServer();
  ASSERT_NE(addr_a.Port(), 0u);
  ASSERT_NE(addr_b.Port(), 0u);

  auto ch_a2b = host_a.network.ConnectRudp(addr_b);
  ASSERT_TRUE(ch_a2b.HasValue());
  host_a.app.PeerRegistryForTest().InsertForTest(addr_b, *ch_a2b);
  auto ch_b2a = host_b.network.ConnectRudp(addr_a);
  ASSERT_TRUE(ch_b2a.HasValue());
  host_b.app.PeerRegistryForTest().InsertForTest(addr_a, *ch_b2a);

  // Source space on A holds the Real.
  const SpaceID kSrcSpace = 1;
  host_a.app.OnCreateSpace({}, nullptr, cellapp::CreateSpace{kSrcSpace});
  auto* space_a = host_a.app.FindSpace(kSrcSpace);
  ASSERT_NE(space_a, nullptr);
  BSPTree tree_a;
  tree_a.InitSingleCell(CellInfo{/*cell_id=*/1, addr_a, /*bounds=*/{}, 0.f, 0});
  space_a->SetBspTree(std::move(tree_a));
  space_a->AddLocalCell(std::make_unique<Cell>(*space_a, /*cell_id=*/1, CellBounds{}));

  cellapp::CreateCellEntity cce;
  cce.entity_id = 100;
  cce.type_id = 1;
  cce.space_id = kSrcSpace;
  cce.position = {-5.f, 0.f, 0.f};
  cce.direction = {1.f, 0.f, 0.f};
  host_a.app.OnCreateCellEntity({}, nullptr, cce);
  ASSERT_TRUE(host_a.app.FindRealEntity(100) != nullptr);

  // Destination space on B must already be hosted with a local cell so the
  // teleport guard accepts it and the BSP re-localizes the arrival.
  const SpaceID kDstSpace = 2;
  host_b.app.OnCreateSpace({}, nullptr, cellapp::CreateSpace{kDstSpace});
  auto* space_b = host_b.app.FindSpace(kDstSpace);
  ASSERT_NE(space_b, nullptr);
  BSPTree tree_b;
  tree_b.InitSingleCell(CellInfo{/*cell_id=*/1, addr_b, /*bounds=*/{}, 0.f, 0});
  space_b->SetBspTree(std::move(tree_b));
  space_b->AddLocalCell(std::make_unique<Cell>(*space_b, /*cell_id=*/1, CellBounds{}));

  // Stub the mgr round-trip: seed the pending teleport, then feed the reply
  // naming B as the destination host.
  const math::Vector3 dst_pos{12.f, 0.f, 9.f};
  host_a.app.PendingTeleportsForTest()[1] =
      CellApp::PendingTeleport{kDstSpace, dst_pos, {1.f, 0.f, 0.f}, Clock::now()};
  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = 1;
  reply.entity_id = 100;
  reply.space_id = kDstSpace;
  reply.found = true;
  reply.host_addr = addr_b;
  reply.cell_id = 1;
  host_a.app.OnResolveSpaceHostReply(addr_b, nullptr, reply);

  ASSERT_TRUE(PumpUntil(host_a, host_b, [&] {
    auto* on_b = host_b.app.FindEntity(100);
    return on_b != nullptr && on_b->IsReal();
  })) << "B never rehydrated the teleported Real";
  ASSERT_TRUE(
      PumpUntil(host_a, host_b, [&] { return host_a.app.PendingOffloadsForTest().empty(); }))
      << "A's pending_offloads_ never drained after teleport";

  auto* on_a = host_a.app.FindEntity(100);
  ASSERT_NE(on_a, nullptr);
  EXPECT_TRUE(on_a->IsGhost());

  auto* on_b = host_b.app.FindEntity(100);
  ASSERT_NE(on_b, nullptr);
  EXPECT_TRUE(on_b->IsReal());
  EXPECT_EQ(on_b->GetSpace().Id(), kDstSpace);
  EXPECT_FLOAT_EQ(on_b->Position().x, dst_pos.x);
  EXPECT_FLOAT_EQ(on_b->Position().z, dst_pos.z);
}

}  // namespace
}  // namespace atlas
