// CellAppMgr wire-level integration.
//
// Runs a real CellAppMgr in a background thread and drives it with
// synthetic CellApp clients over real RUDP. This covers the Register →
// InformCellLoad → CreateSpaceRequest → AddCellToSpace + UpdateGeometry
// pipeline end-to-end — the same wire bytes a real atlas_cellapp would
// exchange.
//
// Follows the `test_baseappmgr_registration.cpp` pattern: threaded
// ManagerApp + synthetic NetworkInterface clients, no process spawning.

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cellapp/intercell_messages.h"
#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/socket.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::cellappmgr;

namespace {

template <typename Pred>
bool PollUntil(EventDispatcher& disp, Pred pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

auto ReserveUdpPort() -> uint16_t {
  auto sock = Socket::CreateUdp();
  EXPECT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto local = sock->LocalAddress();
  return local ? local->Port() : 0;
}

struct CellAppMgrArgv {
  explicit CellAppMgrArgv(uint16_t internal_port)
      : storage{"cellappmgr", "--type",          "cellappmgr",
                "--name",     "cellappmgr_test", "--update-hertz",
                "100",        "--internal-port", std::to_string(internal_port),
                "--machined", "127.0.0.1:1"} {
    for (auto& s : storage) ptrs.push_back(s.data());
  }
  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
  std::vector<std::string> storage;
  std::vector<char*> ptrs;
};

// Test-only subclass that signals when Init completes and drives the
// shutdown flag from OnTickComplete — same shape as BaseAppMgr's test.
class TestCellAppMgr final : public CellAppMgr {
 public:
  TestCellAppMgr(EventDispatcher& dispatcher, NetworkInterface& network,
                 std::promise<Address>& addr_promise, std::atomic<bool>& stop_flag)
      : CellAppMgr(dispatcher, network), addr_promise_(addr_promise), stop_flag_(stop_flag) {}

 protected:
  auto Init(int argc, char* argv[]) -> bool override {
    if (!CellAppMgr::Init(argc, argv)) return false;
    Address addr = Network().RudpAddress();
    if (addr.Ip() == 0) addr = Address("127.0.0.1", addr.Port());
    addr_promise_.set_value(addr);
    return true;
  }
  void OnTickComplete() override {
    CellAppMgr::OnTickComplete();
    if (stop_flag_.load(std::memory_order_acquire)) Shutdown();
  }

 private:
  std::promise<Address>& addr_promise_;
  std::atomic<bool>& stop_flag_;
};

struct CellAppClient {
  explicit CellAppClient(std::string name) : dispatcher(std::move(name)), network(dispatcher) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    network.InterfaceTable().RegisterTypedHandler<RegisterCellAppAck>(
        [this](const Address&, Channel*, const RegisterCellAppAck& msg) {
          register_ack = msg;
          register_ack_received.store(true, std::memory_order_release);
        });
    network.InterfaceTable().RegisterTypedHandler<AddCellToSpace>(
        [this](const Address&, Channel*, const AddCellToSpace& msg) {
          add_cell_msgs.push_back(msg);
        });
    network.InterfaceTable().RegisterTypedHandler<UpdateGeometry>(
        [this](const Address&, Channel*, const UpdateGeometry& msg) {
          update_geometry_msgs.push_back(msg);
        });
    network.InterfaceTable().RegisterTypedHandler<SpaceCreatedResult>(
        [this](const Address&, Channel*, const SpaceCreatedResult& msg) {
          space_created_results.push_back(msg);
        });
  }

  EventDispatcher dispatcher;
  NetworkInterface network;
  std::atomic<bool> register_ack_received{false};
  RegisterCellAppAck register_ack;
  std::vector<AddCellToSpace> add_cell_msgs;
  std::vector<UpdateGeometry> update_geometry_msgs;
  std::vector<SpaceCreatedResult> space_created_results;
};

struct MgrFixture {
  uint16_t port{0};
  std::promise<Address> addr_promise;
  std::future<Address> addr_future;
  std::atomic<bool> stop_flag{false};
  std::thread thread;
  Address server_addr;

  MgrFixture() : addr_future(addr_promise.get_future()) {
    port = ReserveUdpPort();
    if (port == 0) return;

    thread = std::thread([this] {
      EventDispatcher disp{"cellappmgr_server"};
      disp.SetMaxPollWait(Milliseconds(1));
      NetworkInterface net(disp);
      TestCellAppMgr app(disp, net, addr_promise, stop_flag);
      CellAppMgrArgv args(port);
      EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 0);
    });

    server_addr = addr_future.get();
  }

  ~MgrFixture() {
    stop_flag.store(true, std::memory_order_release);
    if (thread.joinable()) thread.join();
  }
};

}  // namespace

// ============================================================================
// Register two CellApps, verify they get distinct app_ids over the wire.
// ============================================================================

TEST(CellAppMgrIntegration, TwoClients_RegisterOverRudp_DistinctAppIds) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);
  ASSERT_NE(fx.server_addr.Port(), 0u);

  CellAppClient a{"cellapp_a"};
  CellAppClient b{"cellapp_b"};
  auto ch_a = a.network.ConnectRudp(fx.server_addr);
  auto ch_b = b.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_a.HasValue()) << ch_a.Error().Message();
  ASSERT_TRUE(ch_b.HasValue()) << ch_b.Error().Message();

  RegisterCellApp reg_a;
  reg_a.internal_addr = Address(0, 30001);
  ASSERT_TRUE((*ch_a)->SendMessage(reg_a).HasValue());
  ASSERT_TRUE(PollUntil(a.dispatcher, [&] {
    return a.register_ack_received.load(std::memory_order_acquire);
  })) << "CellApp A register ack not received";
  EXPECT_TRUE(a.register_ack.success);
  EXPECT_EQ(a.register_ack.app_id, 1u);

  RegisterCellApp reg_b;
  reg_b.internal_addr = Address(0, 30002);
  ASSERT_TRUE((*ch_b)->SendMessage(reg_b).HasValue());
  ASSERT_TRUE(PollUntil(b.dispatcher,
                        [&] { return b.register_ack_received.load(std::memory_order_acquire); }));
  EXPECT_TRUE(b.register_ack.success);
  EXPECT_EQ(b.register_ack.app_id, 2u);
}

// ============================================================================
// Review-fix S2/S3: CreateSpaceRequest replies to the sender with
// SpaceCreatedResult carrying success + host_addr + cell_id.
// ============================================================================

TEST(CellAppMgrIntegration, CreateSpace_RepliesWithSpaceCreatedResult) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  // Host CellApp registers so the mgr has somewhere to put the Space.
  CellAppClient host{"space_host"};
  ASSERT_TRUE(host.network.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  auto ch_host = host.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_host.HasValue());
  RegisterCellApp host_reg;
  host_reg.internal_addr = Address(0, 31001);
  ASSERT_TRUE((*ch_host)->SendMessage(host_reg).HasValue());
  ASSERT_TRUE(PollUntil(
      host.dispatcher, [&] { return host.register_ack_received.load(std::memory_order_acquire); }));

  // Requester client (simulates BaseApp). Send a CreateSpaceRequest
  // with reply_addr pointing at our own RUDP listener so the mgr's
  // ConnectRudpNocwnd reply path has somewhere to land.
  CellAppClient requester{"space_requester"};
  ASSERT_TRUE(requester.network.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  auto ch_req = requester.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_req.HasValue());

  CreateSpaceRequest csr;
  csr.space_id = 123;
  csr.request_id = 77;
  csr.reply_addr = requester.network.RudpAddress();
  ASSERT_TRUE((*ch_req)->SendMessage(csr).HasValue());

  ASSERT_TRUE(PollUntil(requester.dispatcher, [&] {
    return !requester.space_created_results.empty();
  })) << "CellAppMgr did not reply with SpaceCreatedResult";

  const auto& reply = requester.space_created_results[0];
  EXPECT_EQ(reply.request_id, 77u);
  EXPECT_EQ(reply.space_id, 123u);
  EXPECT_TRUE(reply.success);
  EXPECT_GT(reply.cell_id, 0u);
  EXPECT_EQ(reply.host_addr.Port(), 31001u);  // advertised port of `host`
}

TEST(CellAppMgrIntegration, CreateSpace_NoHosts_RepliesWithFailure) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  CellAppClient requester{"space_requester_nohosts"};
  ASSERT_TRUE(requester.network.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  auto ch_req = requester.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_req.HasValue());

  CreateSpaceRequest csr;
  csr.space_id = 456;
  csr.request_id = 88;
  csr.reply_addr = requester.network.RudpAddress();
  ASSERT_TRUE((*ch_req)->SendMessage(csr).HasValue());

  ASSERT_TRUE(PollUntil(requester.dispatcher, [&] {
    return !requester.space_created_results.empty();
  })) << "Expected failure reply when no CellApps are registered";
  const auto& reply = requester.space_created_results[0];
  EXPECT_EQ(reply.request_id, 88u);
  EXPECT_FALSE(reply.success);
}

// ============================================================================
// CreateSpaceRequest triggers AddCellToSpace + UpdateGeometry on the host.
// ============================================================================

TEST(CellAppMgrIntegration, CreateSpace_PushesAddCellAndGeometryToHost) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  CellAppClient a{"cellapp_a_space"};
  auto ch_a = a.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_a.HasValue());

  RegisterCellApp reg_a;
  reg_a.internal_addr = Address(0, 30101);
  ASSERT_TRUE((*ch_a)->SendMessage(reg_a).HasValue());
  ASSERT_TRUE(PollUntil(a.dispatcher,
                        [&] { return a.register_ack_received.load(std::memory_order_acquire); }));

  CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.request_id = 1;
  csr.reply_addr = a.network.RudpAddress();
  ASSERT_TRUE((*ch_a)->SendMessage(csr).HasValue());

  // Wait for BOTH AddCellToSpace and UpdateGeometry to land on the host.
  ASSERT_TRUE(PollUntil(a.dispatcher, [&] {
    return !a.add_cell_msgs.empty() && !a.update_geometry_msgs.empty();
  })) << "Host did not receive geometry messages";

  ASSERT_EQ(a.add_cell_msgs.size(), 1u);
  EXPECT_EQ(a.add_cell_msgs[0].space_id, 42u);
  EXPECT_GT(a.add_cell_msgs[0].cell_id, 0u);  // id pool is 1-based.

  ASSERT_FALSE(a.update_geometry_msgs.empty());
  EXPECT_EQ(a.update_geometry_msgs[0].space_id, 42u);
  EXPECT_FALSE(a.update_geometry_msgs[0].bsp_blob.empty());

  // The blob must deserialise and resolve to a single leaf on the
  // ADVERTISED port (reg_a.internal_addr.Port() = 30101). The client's
  // actual listener port is whatever the OS picked — unrelated to what
  // the mgr tracks in its routing table.
  BinaryReader r(std::span<const std::byte>(a.update_geometry_msgs[0].bsp_blob));
  auto tree = BSPTree::Deserialize(r);
  ASSERT_TRUE(tree.HasValue());
  auto leaves = tree->Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr.Port(), 30101u);
}

// ============================================================================
// InformCellLoad reaches the mgr over the wire and influences host pick.
// ============================================================================

TEST(CellAppMgrIntegration, InformCellLoad_InfluencesLeastLoadedHostPick) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  CellAppClient a{"cellapp_load_a"};
  CellAppClient b{"cellapp_load_b"};
  auto ch_a = a.network.ConnectRudp(fx.server_addr);
  auto ch_b = b.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_a.HasValue());
  ASSERT_TRUE(ch_b.HasValue());

  RegisterCellApp reg_a;
  reg_a.internal_addr = Address(0, 30201);
  ASSERT_TRUE((*ch_a)->SendMessage(reg_a).HasValue());
  ASSERT_TRUE(PollUntil(a.dispatcher,
                        [&] { return a.register_ack_received.load(std::memory_order_acquire); }));
  RegisterCellApp reg_b;
  reg_b.internal_addr = Address(0, 30202);
  ASSERT_TRUE((*ch_b)->SendMessage(reg_b).HasValue());
  ASSERT_TRUE(PollUntil(b.dispatcher,
                        [&] { return b.register_ack_received.load(std::memory_order_acquire); }));

  // Make `a` the hot one.
  InformCellLoad load_a;
  load_a.app_id = a.register_ack.app_id;
  load_a.load = 0.9f;
  load_a.entity_count = 1000;
  ASSERT_TRUE((*ch_a)->SendMessage(load_a).HasValue());
  InformCellLoad load_b;
  load_b.app_id = b.register_ack.app_id;
  load_b.load = 0.1f;
  load_b.entity_count = 10;
  ASSERT_TRUE((*ch_b)->SendMessage(load_b).HasValue());

  // Give the mgr a beat to ingest the load reports before the space
  // request races in. Deterministic because InformCellLoad is applied
  // synchronously on the mgr thread when it dequeues the packet, but
  // our own poller needs time to yield for both.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CreateSpaceRequest csr;
  csr.space_id = 7;
  csr.request_id = 1;
  ASSERT_TRUE((*ch_a)->SendMessage(csr).HasValue());

  // The lightly-loaded client (b) should have received AddCellToSpace.
  ASSERT_TRUE(PollUntil(b.dispatcher, [&] { return !b.add_cell_msgs.empty(); }))
      << "lightly-loaded CellApp did not receive the new space";
  EXPECT_EQ(b.add_cell_msgs[0].space_id, 7u);

  // And the heavy one (a) should NOT — sanity check to ensure the mgr
  // actually routed based on load rather than insertion order.
  a.dispatcher.ProcessOnce();
  EXPECT_TRUE(a.add_cell_msgs.empty());
}
