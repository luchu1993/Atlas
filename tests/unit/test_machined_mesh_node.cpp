#include "server/machined_mesh_node.h"

#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "foundation/clock.h"
#include "foundation/process_type.h"
#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/mesh_gossip.h"
#include "serialization/binary_stream.h"
#include "server/mesh_transport.h"

using namespace atlas;

template <typename Pred>
static bool poll_until(EventDispatcher& dispatcher, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    dispatcher.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// Pumps the dispatcher so in-flight HELLO datagrams reach peer callbacks.
static void Pump(EventDispatcher& dispatcher, std::chrono::milliseconds dur) {
  poll_until(dispatcher, [] { return false; }, dur);
}

static auto Mesh(uint16_t port) -> Address { return Address("127.0.0.1", port); }

TEST(MachinedMeshNode, TwoNodesDiscoverThenDetectBuddyDeath) {
  EventDispatcher disp{"mesh_node_test"};
  disp.SetMaxPollWait(Milliseconds(1));

  // Canonical mesh identities (carried in the HELLO) are distinct from the
  // ephemeral sockets the nodes actually bind for the test.
  MachinedMeshNode a(disp, Mesh(7001));
  MachinedMeshNode b(disp, Mesh(7002));
  const auto timeout = std::chrono::duration_cast<Duration>(std::chrono::seconds(1));
  a.SetPeerTimeout(timeout);
  b.SetPeerTimeout(timeout);

  ASSERT_TRUE(a.Open(Mesh(0), Mesh(0), /*incarnation=*/100).HasValue());
  ASSERT_TRUE(b.Open(Mesh(0), Mesh(0), /*incarnation=*/200).HasValue());
  // Directed delivery makes the exchange deterministic; real machined broadcasts
  // to 255.255.255.255 instead.
  a.SetBroadcastTarget(b.BoundAddress());
  b.SetBroadcastTarget(a.BoundAddress());

  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  a.Tick(t0);  // each broadcasts its HELLO
  b.Tick(t0);
  Pump(disp, std::chrono::milliseconds(60));
  a.Tick(t0);  // drain queued HELLOs into the ring
  b.Tick(t0);

  EXPECT_EQ(a.PeerCount(), 1u);
  EXPECT_EQ(b.PeerCount(), 1u);
  ASSERT_TRUE(a.Buddy().has_value());
  EXPECT_EQ(*a.Buddy(), Mesh(7002));

  // b goes silent; once its last HELLO ages past the timeout, a (its ring
  // predecessor) owns announcing b's death.
  std::optional<Address> dead;
  a.SetDeathCallback([&](const Address& d) { dead = d; });
  const auto t1 = t0 + std::chrono::seconds(2);
  a.Tick(t1);

  ASSERT_TRUE(dead.has_value());
  EXPECT_EQ(*dead, Mesh(7002));
  EXPECT_EQ(a.PeerCount(), 0u);
}

TEST(MachinedMeshNode, RestartedPeerReportsRestartObservation) {
  EventDispatcher disp{"mesh_node_test"};
  disp.SetMaxPollWait(Milliseconds(1));

  MachinedMeshNode a(disp, Mesh(7001));
  MachinedMeshNode b(disp, Mesh(7002));
  ASSERT_TRUE(a.Open(Mesh(0), Mesh(0), 100).HasValue());
  ASSERT_TRUE(b.Open(Mesh(0), Mesh(0), 200).HasValue());
  a.SetBroadcastTarget(b.BoundAddress());
  b.SetBroadcastTarget(a.BoundAddress());

  std::vector<MachinedMesh::Observation> events;
  a.SetPeerEventCallback([&](const Address&, MachinedMesh::Observation o) { events.push_back(o); });

  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  b.Tick(t0);  // b announces incarnation 200
  Pump(disp, std::chrono::milliseconds(60));
  a.Tick(t0);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], MachinedMesh::Observation::kNew);

  // b restarts with a fresh incarnation; a must see it as a restart so it can
  // resync registry state toward the rejoined peer.
  MachinedMeshNode b2(disp, Mesh(7002));
  ASSERT_TRUE(b2.Open(Mesh(0), Mesh(0), 201).HasValue());
  b2.SetBroadcastTarget(a.BoundAddress());
  const auto t1 = t0 + std::chrono::milliseconds(100);
  b2.Tick(t1);
  Pump(disp, std::chrono::milliseconds(60));
  a.Tick(t1);

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[1], MachinedMesh::Observation::kRestarted);
}

TEST(MachinedMeshNode, ReceivesRegistryGossip) {
  EventDispatcher disp{"mesh_node_test"};
  disp.SetMaxPollWait(Milliseconds(1));
  MachinedMeshNode node(disp, Mesh(7001));
  ASSERT_TRUE(node.Open(Mesh(0), Mesh(0), 100).HasValue());
  MeshTransport sender(disp);
  ASSERT_TRUE(sender.Open(Mesh(0), Mesh(0)).HasValue());

  std::optional<Address> got_owner;
  std::vector<machined::ProcessInfo> got_procs;
  node.SetRegistryCallback([&](const Address& owner, std::vector<machined::ProcessInfo> procs) {
    got_owner = owner;
    got_procs = std::move(procs);
  });

  machined::MeshRegistryMsg msg;
  msg.owner = Mesh(7002);
  machined::ProcessInfo p;
  p.process_type = ProcessType::kCellApp;
  p.name = "cellapp-1";
  p.pid = 5;
  msg.processes.push_back(p);
  BinaryWriter w;
  msg.Serialize(w);
  ASSERT_TRUE(sender.SendTo(node.BoundAddress(), w.Data()).HasValue());

  ASSERT_TRUE(poll_until(disp, [&] { return got_owner.has_value(); }));
  EXPECT_EQ(*got_owner, Mesh(7002));
  ASSERT_EQ(got_procs.size(), 1u);
  EXPECT_EQ(got_procs[0].name, "cellapp-1");
}

TEST(MachinedMeshNode, IgnoresOwnLoopedBackRegistryGossip) {
  EventDispatcher disp{"mesh_node_test"};
  disp.SetMaxPollWait(Milliseconds(1));
  MachinedMeshNode node(disp, Mesh(7001));
  ASSERT_TRUE(node.Open(Mesh(0), Mesh(0), 100).HasValue());
  MeshTransport sender(disp);
  ASSERT_TRUE(sender.Open(Mesh(0), Mesh(0)).HasValue());

  int callbacks = 0;
  node.SetRegistryCallback([&](const Address&, std::vector<machined::ProcessInfo>) { ++callbacks; });

  // A registry datagram whose owner is THIS node is its own broadcast looping
  // back; folding it would double-count local processes on the next query.
  machined::MeshRegistryMsg msg;
  msg.owner = Mesh(7001);  // == node's self_
  machined::ProcessInfo p;
  p.process_type = ProcessType::kCellApp;
  p.name = "cellapp-self";
  p.pid = 9;
  msg.processes.push_back(p);
  BinaryWriter w;
  msg.Serialize(w);
  ASSERT_TRUE(sender.SendTo(node.BoundAddress(), w.Data()).HasValue());

  Pump(disp, std::chrono::milliseconds(80));
  EXPECT_EQ(callbacks, 0);
}

TEST(MachinedMeshNode, DeathDetectionBroadcastsToPeers) {
  EventDispatcher disp{"mesh_node_test"};
  disp.SetMaxPollWait(Milliseconds(1));
  MachinedMeshNode a(disp, Mesh(7001));
  MachinedMeshNode b(disp, Mesh(7002));
  MachinedMeshNode c(disp, Mesh(7003));
  a.SetPeerTimeout(std::chrono::duration_cast<Duration>(std::chrono::seconds(1)));
  ASSERT_TRUE(a.Open(Mesh(0), Mesh(0), 1).HasValue());
  ASSERT_TRUE(b.Open(Mesh(0), Mesh(0), 2).HasValue());
  ASSERT_TRUE(c.Open(Mesh(0), Mesh(0), 3).HasValue());
  b.SetBroadcastTarget(a.BoundAddress());  // b's HELLO reaches a
  a.SetBroadcastTarget(c.BoundAddress());  // a's death announcement reaches c

  std::optional<Address> c_saw_dead;
  c.SetDeathCallback([&](const Address& d) { c_saw_dead = d; });

  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  b.Tick(t0);
  Pump(disp, std::chrono::milliseconds(60));
  a.Tick(t0);
  ASSERT_EQ(a.PeerCount(), 1u);

  // b goes silent; a (its ring predecessor) detects + announces the death, which
  // reaches c even though c never monitored b.
  const auto t1 = t0 + std::chrono::seconds(2);
  a.Tick(t1);
  Pump(disp, std::chrono::milliseconds(60));

  ASSERT_TRUE(c_saw_dead.has_value());
  EXPECT_EQ(*c_saw_dead, Mesh(7002));
}
