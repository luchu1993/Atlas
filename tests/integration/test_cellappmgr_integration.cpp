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
#include <memory>
#include <set>
#include <string>
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
        [this](const Address&, Channel* ch, const AddCellToSpace& msg) {
          add_cell_msgs.push_back(msg);
          // Mirror real cellapp: ack mgr so deferred UpdateGeometry can release.
          // Tests that exercise the timeout fallback set ack_add_cell=false.
          if (ack_add_cell && ch != nullptr) {
            AddCellToSpaceAck ack;
            ack.space_id = msg.space_id;
            ack.cell_id = msg.cell_id;
            ack.success = true;
            (void)ch->SendMessage(ack);
          }
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
  // Real cellapp acks AddCellToSpace; some tests opt out to exercise the
  // mgr-side timeout-fallback broadcast.
  bool ack_add_cell{true};
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

TEST(CellAppMgrIntegration, CreateSpace_NoHosts_QueuesUntilCellAppRegisters) {
  // Engine-spawned space master semantics (BigWorld-style): a request that
  // beats the first cellapp registration is parked, not failed. The reply
  // lands after the late cellapp comes online — letting BaseApp pre-register
  // a space master at startup without racing the cellapp boot order.
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

  // Drain a couple of ticks; nothing should arrive yet because no cellapp is up.
  for (int i = 0; i < 20; ++i) {
    requester.dispatcher.ProcessOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(requester.space_created_results.empty())
      << "CreateSpaceRequest must not resolve before a CellApp registers";

  // Late host registration drains the queued request → reply now flows.
  CellAppClient host{"cellapp_late_host"};
  auto ch_h = host.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_h.HasValue());
  RegisterCellApp reg;
  reg.internal_addr = Address(0, 30501);
  ASSERT_TRUE((*ch_h)->SendMessage(reg).HasValue());

  ASSERT_TRUE(PollUntil(requester.dispatcher, [&] {
    return !requester.space_created_results.empty();
  })) << "Queued CreateSpaceRequest never drained after cellapp registered";
  const auto& reply = requester.space_created_results[0];
  EXPECT_EQ(reply.request_id, 88u);
  EXPECT_TRUE(reply.success);
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

// Closes the boot-time race where the queued CreateSpace drained on the first
// cellapp and sibling cellapps registered moments later.
TEST(CellAppMgrIntegration, LateCellAppRegistration_TriggersElasticSplit) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  // Phase 1: one cellapp + a Space create → single-cell Space.
  CellAppClient first{"cellapp_first"};
  auto ch_first = first.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_first.HasValue());
  RegisterCellApp reg_first;
  reg_first.internal_addr = Address(0, 30601);
  ASSERT_TRUE((*ch_first)->SendMessage(reg_first).HasValue());
  ASSERT_TRUE(PollUntil(first.dispatcher, [&] {
    return first.register_ack_received.load(std::memory_order_acquire);
  }));

  CreateSpaceRequest csr;
  csr.space_id = 77;
  csr.request_id = 1;
  csr.reply_addr = first.network.RudpAddress();
  ASSERT_TRUE((*ch_first)->SendMessage(csr).HasValue());

  ASSERT_TRUE(PollUntil(first.dispatcher, [&] {
    return !first.add_cell_msgs.empty() && !first.update_geometry_msgs.empty();
  }));
  EXPECT_EQ(first.add_cell_msgs.size(), 1u);
  BinaryReader r1(std::span<const std::byte>(first.update_geometry_msgs.back().bsp_blob));
  auto tree1 = BSPTree::Deserialize(r1);
  ASSERT_TRUE(tree1.HasValue());
  EXPECT_EQ(tree1->Leaves().size(), 1u);

  // Phase 2: a late cellapp registers; the mgr must split the existing cell
  // onto it and broadcast the 2-leaf geometry to everyone in the Space.
  CellAppClient late{"cellapp_late"};
  auto ch_late = late.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_late.HasValue());
  RegisterCellApp reg_late;
  reg_late.internal_addr = Address(0, 30602);
  ASSERT_TRUE((*ch_late)->SendMessage(reg_late).HasValue());
  ASSERT_TRUE(PollUntil(late.dispatcher, [&] {
    return late.register_ack_received.load(std::memory_order_acquire);
  }));

  // Late app receives an AddCellToSpace for the freshly-split leaf.
  ASSERT_TRUE(PollUntil(late.dispatcher, [&] {
    return !late.add_cell_msgs.empty() && !late.update_geometry_msgs.empty();
  })) << "Late cellapp never received the elastic-split AddCell+UpdateGeometry";
  EXPECT_EQ(late.add_cell_msgs[0].space_id, 77u);
  EXPECT_FALSE(late.add_cell_msgs[0].is_primary)
      << "Late-joining cellapp must NOT be the primary host";

  // First app sees the geometry refresh — its cell now covers only half.
  ASSERT_TRUE(PollUntil(first.dispatcher, [&] {
    return first.update_geometry_msgs.size() >= 2u;
  })) << "Primary cellapp never received the updated 2-leaf geometry";
  BinaryReader r2(
      std::span<const std::byte>(first.update_geometry_msgs.back().bsp_blob));
  auto tree2 = BSPTree::Deserialize(r2);
  ASSERT_TRUE(tree2.HasValue());
  ASSERT_EQ(tree2->Leaves().size(), 2u);

  // Two distinct cellapp_addr entries — the split actually fanned out.
  std::set<uint16_t> ports;
  for (auto* leaf : tree2->Leaves()) ports.insert(leaf->cellapp_addr.Port());
  EXPECT_EQ(ports.size(), 2u);
}

// Silent receiver: AddCellToSpace lands but the ack is dropped — the mgr's
// timeout fallback broadcasts UpdateGeometry anyway so cluster keeps moving.
TEST(CellAppMgrIntegration, LateCellAppNotAcking_TimeoutFallbackBroadcasts) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  CellAppClient first{"cellapp_first_to_grow"};
  auto ch_first = first.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_first.HasValue());
  RegisterCellApp reg_first;
  reg_first.internal_addr = Address(0, 30701);
  ASSERT_TRUE((*ch_first)->SendMessage(reg_first).HasValue());
  ASSERT_TRUE(PollUntil(first.dispatcher, [&] {
    return first.register_ack_received.load(std::memory_order_acquire);
  }));

  CreateSpaceRequest csr;
  csr.space_id = 88;
  csr.request_id = 1;
  csr.reply_addr = first.network.RudpAddress();
  ASSERT_TRUE((*ch_first)->SendMessage(csr).HasValue());
  ASSERT_TRUE(PollUntil(first.dispatcher, [&] {
    return !first.update_geometry_msgs.empty();
  }));

  // Silent client: receives AddCellToSpace but never sends the ack.
  CellAppClient silent{"cellapp_silent"};
  silent.ack_add_cell = false;
  auto ch_silent = silent.network.ConnectRudp(fx.server_addr);
  ASSERT_TRUE(ch_silent.HasValue());
  RegisterCellApp reg_silent;
  reg_silent.internal_addr = Address(0, 30702);
  ASSERT_TRUE((*ch_silent)->SendMessage(reg_silent).HasValue());

  // Silent client receives AddCellToSpace immediately — the mgr sends it
  // before deferring the geometry broadcast.
  ASSERT_TRUE(PollUntil(silent.dispatcher, [&] {
    return !silent.add_cell_msgs.empty();
  })) << "Silent receiver missed AddCellToSpace";

  // At silent.add_cell arrival, the post-Split geometry must still be
  // parked — message ordering proves the deferral without a timed wait.
  const auto baseline_count = first.update_geometry_msgs.size();
  EXPECT_EQ(baseline_count, 1u)
      << "first should have seen exactly 1 UpdateGeometry (from initial CreateSpace) "
         "at the moment silent gets AddCellToSpace";

  // After ~500ms timeout fires, mgr falls back to broadcasting anyway.
  // Allow generous wall-clock budget for the fallback to land.
  ASSERT_TRUE(PollUntil(
      first.dispatcher,
      [&] { return first.update_geometry_msgs.size() > baseline_count; },
      std::chrono::milliseconds(3000)))
      << "timeout fallback never broadcast geometry after the ack window";

  // The fallback-broadcast BSP must reflect the new 2-leaf state.
  BinaryReader r(std::span<const std::byte>(first.update_geometry_msgs.back().bsp_blob));
  auto tree = BSPTree::Deserialize(r);
  ASSERT_TRUE(tree.HasValue());
  EXPECT_EQ(tree->Leaves().size(), 2u);
}

TEST(CellAppMgrIntegration, CreateSpace_MultiCellBootstrap_DistributesAcrossHosts) {
  MgrFixture fx;
  ASSERT_NE(fx.port, 0u);

  constexpr int kN = 4;
  std::vector<std::unique_ptr<CellAppClient>> clients;
  std::vector<ReliableUdpChannel*> channels;
  for (int i = 0; i < kN; ++i) {
    clients.emplace_back(std::make_unique<CellAppClient>("cellapp_multi_" + std::to_string(i)));
    auto ch = clients.back()->network.ConnectRudp(fx.server_addr);
    ASSERT_TRUE(ch.HasValue());
    channels.push_back(*ch);
    RegisterCellApp reg;
    reg.internal_addr = Address(0, static_cast<uint16_t>(30401 + i));
    ASSERT_TRUE((*ch)->SendMessage(reg).HasValue());
  }
  for (auto& c : clients) {
    ASSERT_TRUE(PollUntil(c->dispatcher,
                          [&] { return c->register_ack_received.load(std::memory_order_acquire); }))
        << "register ack stuck";
  }

  CreateSpaceRequest csr;
  csr.space_id = 99;
  csr.request_id = 1;
  csr.reply_addr = clients[0]->network.RudpAddress();
  csr.initial_cell_count = kN;
  ASSERT_TRUE(channels[0]->SendMessage(csr).HasValue());

  // Every host should receive AddCellToSpace + the same UpdateGeometry.
  for (auto& c : clients) {
    ASSERT_TRUE(PollUntil(c->dispatcher, [&] {
      return !c->add_cell_msgs.empty() && !c->update_geometry_msgs.empty();
    })) << "a host missed AddCell or UpdateGeometry";
    EXPECT_EQ(c->add_cell_msgs.size(), 1u);
    EXPECT_EQ(c->add_cell_msgs[0].space_id, 99u);
  }

  // Collect the cell_ids reported across hosts; they must be distinct.
  std::set<cellappmgr::CellID> reported_cells;
  for (auto& c : clients) reported_cells.insert(c->add_cell_msgs[0].cell_id);
  EXPECT_EQ(reported_cells.size(), static_cast<std::size_t>(kN));

  // Deserialised BSP must hold exactly N leaves with N distinct cellapp
  // addresses — the bootstrap really fanned out.
  BinaryReader r(std::span<const std::byte>(clients[0]->update_geometry_msgs.back().bsp_blob));
  auto tree = BSPTree::Deserialize(r);
  ASSERT_TRUE(tree.HasValue());
  auto leaves = tree->Leaves();
  ASSERT_EQ(leaves.size(), static_cast<std::size_t>(kN));
  std::set<uint16_t> ports;
  for (auto* leaf : leaves) ports.insert(leaf->cellapp_addr.Port());
  EXPECT_EQ(ports.size(), static_cast<std::size_t>(kN));
}
