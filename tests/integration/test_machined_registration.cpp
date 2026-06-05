#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "machined/machined_app.h"
#include "network/event_dispatcher.h"
#include "network/machined_types.h"
#include "network/mesh_gossip.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"
#include "server/common_messages.h"
#include "server/machined_client.h"
#include "server/mesh_transport.h"
#include "server/server_config.h"

using namespace atlas;
using namespace atlas::machined;

template <typename Pred>
static bool poll_until(EventDispatcher& disp, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

template <typename Pred>
static bool poll_until(EventDispatcher& first, EventDispatcher& second, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    first.ProcessOnce();
    second.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

static std::vector<ProcessInfo> wait_for_registry_entry(
    EventDispatcher& disp, MachinedClient& client, ProcessType type,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  std::vector<ProcessInfo> result;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    bool done = false;
    client.QueryAsync(type, [&](std::vector<ProcessInfo> infos) {
      result = std::move(infos);
      done = true;
    });
    poll_until(disp, [&] { return done; }, std::chrono::milliseconds(300));
    if (!result.empty()) break;
  }
  return result;
}

struct MachinedArgv {
  std::vector<std::string> storage{"machined", "--type",          "machined",
                                   "--name",   "machined",        "--update-hertz",
                                   "100",      "--internal-port", "0"};
  std::vector<char*> ptrs;

  MachinedArgv() {
    for (auto& s : storage) ptrs.push_back(s.data());
  }

  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

class TestMachinedApp : public MachinedApp {
 public:
  TestMachinedApp(EventDispatcher& d, NetworkInterface& n, std::promise<Address>& addr_promise,
                  std::atomic<bool>& stop_flag)
      : MachinedApp(d, n), addr_promise_(addr_promise), stop_flag_(stop_flag) {}

 protected:
  auto Init(int argc, char* argv[]) -> bool override {
    if (!MachinedApp::Init(argc, argv)) return false;
    Address addr = Network().TcpAddress();
    if (addr.Ip() == 0) addr = Address("127.0.0.1", addr.Port());
    addr_promise_.set_value(addr);
    return true;
  }

  void OnTickComplete() override {
    MachinedApp::OnTickComplete();
    if (stop_flag_.load(std::memory_order_acquire)) Shutdown();
  }

 private:
  std::promise<Address>& addr_promise_;
  std::atomic<bool>& stop_flag_;
};

class MachinedRegistrationTest : public ::testing::Test {
 protected:
  EventDispatcher client_disp_{"client"};
  NetworkInterface client_ni_{client_disp_};

  Address machined_addr_;

  std::atomic<bool> stop_flag_{false};
  std::thread machined_thread_;

  void SetUp() override {
    client_disp_.SetMaxPollWait(Milliseconds(1));

    std::promise<Address> addr_promise;
    auto addr_future = addr_promise.get_future();

    machined_thread_ =
        std::thread([promise = std::move(addr_promise), &stop_flag = stop_flag_]() mutable {
          EventDispatcher disp("machined");
          NetworkInterface ni(disp);
          TestMachinedApp app(disp, ni, promise, stop_flag);

          MachinedArgv args;
          app.RunApp(args.argc(), args.argv());
        });

    auto status = addr_future.wait_for(std::chrono::seconds(10));
    ASSERT_EQ(status, std::future_status::ready) << "MachinedApp failed to start in time";
    machined_addr_ = addr_future.get();
  }

  void TearDown() override {
    stop_flag_.store(true, std::memory_order_release);
    if (machined_thread_.joinable()) machined_thread_.join();
  }
};

TEST_F(MachinedRegistrationTest, RegisterReceivesAck) {
  MachinedClient client(client_disp_, client_ni_);

  ASSERT_TRUE(client.Connect(machined_addr_))
      << "MachinedClient::connect failed for " << machined_addr_.ToString();
  ASSERT_TRUE(poll_until(client_disp_, [&] { return client.IsConnected(); }))
      << "Client did not connect to MachinedApp";

  ServerConfig cfg;
  cfg.process_type = ProcessType::kBaseApp;
  cfg.process_name = "baseapp-1";
  cfg.internal_port = 7200;
  client.SendRegister(cfg);

  std::vector<ProcessInfo> result;
  bool found = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!found && std::chrono::steady_clock::now() < deadline) {
    bool query_done = false;
    client.QueryAsync(ProcessType::kBaseApp, [&](std::vector<ProcessInfo> infos) {
      result = std::move(infos);
      query_done = true;
    });
    poll_until(client_disp_, [&] { return query_done; }, std::chrono::milliseconds(200));
    if (!result.empty()) found = true;
  }

  ASSERT_TRUE(found) << "Registration never appeared in machined registry";
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].process_type, ProcessType::kBaseApp);
  EXPECT_EQ(result[0].name, "baseapp-1");
  EXPECT_EQ(result[0].internal_addr.Port(), 7200);
}

TEST_F(MachinedRegistrationTest, HeartbeatReceivesAck) {
  MachinedClient client(client_disp_, client_ni_);
  ASSERT_TRUE(client.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client_disp_, [&] { return client.IsConnected(); }));

  ServerConfig cfg;
  cfg.process_type = ProcessType::kBaseApp;
  cfg.process_name = "baseapp-hb";
  cfg.internal_port = 7201;
  client.SendRegister(cfg);

  auto reg_result = wait_for_registry_entry(client_disp_, client, ProcessType::kBaseApp);
  ASSERT_FALSE(reg_result.empty()) << "Registration did not complete before heartbeat test";

  client.SendHeartbeat(0.3f, 10);
  poll_until(client_disp_, [&] { return false; }, std::chrono::milliseconds(100));
}

TEST_F(MachinedRegistrationTest, DeregisterRemovesEntry) {
  MachinedClient client(client_disp_, client_ni_);
  ASSERT_TRUE(client.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client_disp_, [&] { return client.IsConnected(); }));

  ServerConfig cfg;
  cfg.process_type = ProcessType::kBaseApp;
  cfg.process_name = "baseapp-dereg";
  cfg.internal_port = 7202;
  client.SendRegister(cfg);

  auto reg_result = wait_for_registry_entry(client_disp_, client, ProcessType::kBaseApp);
  ASSERT_FALSE(reg_result.empty()) << "Registration must succeed before deregister test";

  client.SendDeregister(cfg);

  poll_until(client_disp_, [&] { return false; }, std::chrono::milliseconds(100));

  std::vector<ProcessInfo> result;
  bool query_done = false;
  client.QueryAsync(ProcessType::kBaseApp, [&](std::vector<ProcessInfo> infos) {
    result = std::move(infos);
    query_done = true;
  });
  ASSERT_TRUE(poll_until(client_disp_, [&] { return query_done; }));
  EXPECT_TRUE(result.empty()) << "Registry should be empty after deregister";
}

TEST_F(MachinedRegistrationTest, ShutdownReasonSurvivesTargetDeregister) {
  EventDispatcher listener_disp{"listener"};
  listener_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface listener_ni(listener_disp);

  MachinedClient listener(listener_disp, listener_ni);
  ASSERT_TRUE(listener.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(listener_disp, [&] { return listener.IsConnected(); }));

  bool death_received = false;
  DeathNotification death;
  listener.Subscribe(ListenerType::kDeath, ProcessType::kBaseApp, nullptr,
                     [&](const DeathNotification& msg) {
                       death = msg;
                       death_received = true;
                     });

  bool shutdown_received = false;
  uint8_t shutdown_reason = 0;
  (void)client_ni_.InterfaceTable().RegisterTypedHandler<msg::ShutdownRequest>(
      [&](const Address&, Channel*, const msg::ShutdownRequest& msg) {
        shutdown_reason = msg.reason;
        shutdown_received = true;
      });

  MachinedClient target(client_disp_, client_ni_);
  ASSERT_TRUE(target.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client_disp_, listener_disp, [&] {
    return target.IsConnected() && listener.IsConnected();
  }));

  ServerConfig cfg;
  cfg.process_type = ProcessType::kBaseApp;
  cfg.process_name = "baseapp-shutdown";
  cfg.internal_port = 7205;
  target.SendRegister(cfg);
  auto reg_result = wait_for_registry_entry(client_disp_, target, ProcessType::kBaseApp);
  ASSERT_FALSE(reg_result.empty()) << "Registration must succeed before shutdown test";

  listener.RequestShutdownTarget(ProcessType::kBaseApp, "baseapp-shutdown", 7);
  ASSERT_TRUE(poll_until(client_disp_, listener_disp, [&] { return shutdown_received; }));
  EXPECT_EQ(shutdown_reason, 7u);

  target.SendDeregister(cfg);
  ASSERT_TRUE(poll_until(client_disp_, listener_disp, [&] { return death_received; }));
  EXPECT_EQ(death.process_type, ProcessType::kBaseApp);
  EXPECT_EQ(death.name, "baseapp-shutdown");
  EXPECT_EQ(death.reason, 7u);
}

TEST_F(MachinedRegistrationTest, DuplicateRegistrationRejected) {
  MachinedClient client_a(client_disp_, client_ni_);
  ASSERT_TRUE(client_a.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client_disp_, [&] { return client_a.IsConnected(); }));

  ServerConfig cfg;
  cfg.process_type = ProcessType::kBaseApp;
  cfg.process_name = "baseapp-dup";
  cfg.internal_port = 7203;
  client_a.SendRegister(cfg);

  auto reg1 = wait_for_registry_entry(client_disp_, client_a, ProcessType::kBaseApp);
  ASSERT_FALSE(reg1.empty()) << "First registration must succeed";

  EventDispatcher client2_disp{"client2"};
  client2_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface client2_ni(client2_disp);

  MachinedClient client_b(client2_disp, client2_ni);
  ASSERT_TRUE(client_b.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client2_disp, [&] { return client_b.IsConnected(); }));
  client_b.SendRegister(cfg);

  poll_until(client2_disp, [&] { return false; }, std::chrono::milliseconds(200));

  std::vector<ProcessInfo> result;
  bool query_done = false;
  client_a.QueryAsync(ProcessType::kBaseApp, [&](std::vector<ProcessInfo> infos) {
    result = std::move(infos);
    query_done = true;
  });
  ASSERT_TRUE(poll_until(client_disp_, [&] { return query_done; }));
  ASSERT_EQ(result.size(), 1u) << "Only one entry expected after duplicate rejection";
  EXPECT_EQ(result[0].name, "baseapp-dup");
}

TEST_F(MachinedRegistrationTest, MultipleProcessesCanRegister) {
  MachinedClient client1(client_disp_, client_ni_);
  ASSERT_TRUE(client1.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client_disp_, [&] { return client1.IsConnected(); }));

  EventDispatcher client2_disp{"client2"};
  client2_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface client2_ni(client2_disp);
  MachinedClient client2(client2_disp, client2_ni);
  ASSERT_TRUE(client2.Connect(machined_addr_));
  ASSERT_TRUE(poll_until(client2_disp, [&] { return client2.IsConnected(); }));

  ServerConfig cfg1;
  cfg1.process_type = ProcessType::kBaseApp;
  cfg1.process_name = "baseapp-multi-1";
  cfg1.internal_port = 7210;
  client1.SendRegister(cfg1);

  ServerConfig cfg2;
  cfg2.process_type = ProcessType::kBaseApp;
  cfg2.process_name = "baseapp-multi-2";
  cfg2.internal_port = 7211;
  client2.SendRegister(cfg2);

  std::vector<ProcessInfo> result;
  auto deadline2 = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  while (result.size() < 2 && std::chrono::steady_clock::now() < deadline2) {
    client2_disp.ProcessOnce();

    bool done = false;
    client1.QueryAsync(ProcessType::kBaseApp, [&](std::vector<ProcessInfo> infos) {
      result = std::move(infos);
      done = true;
    });
    poll_until(client_disp_, [&] { return done; }, std::chrono::milliseconds(300));
  }
  EXPECT_EQ(result.size(), 2u) << "Both BaseApp processes should be registered";
}

TEST(MachinedMeshIntegration, RemoteRegistryGossipResolvesInQuery) {
  // A mesh-enabled machined must fold a peer's gossiped process table into its
  // own query responses. The mesh port is internal_port + 2, so the test needs
  // a fixed internal port rather than the OS-assigned 0.
  constexpr uint16_t kInternalPort = 28850;
  constexpr uint16_t kMeshPort = kInternalPort + 2;

  std::atomic<bool> stop{false};
  std::promise<Address> addr_promise;
  auto addr_future = addr_promise.get_future();
  std::thread machined_thread([&]() {
    EventDispatcher disp("machined_mesh");
    NetworkInterface ni(disp);
    TestMachinedApp app(disp, ni, addr_promise, stop);
    std::vector<std::string> storage{"machined",
                                     "--type",
                                     "machined",
                                     "--name",
                                     "machined",
                                     "--update-hertz",
                                     "100",
                                     "--internal-port",
                                     std::to_string(kInternalPort),
                                     "--mesh-enabled",
                                     "true",
                                     "--mesh-advertise-ip",
                                     "127.0.0.1"};
    std::vector<char*> ptrs;
    for (auto& s : storage) ptrs.push_back(s.data());
    app.RunApp(static_cast<int>(ptrs.size()), ptrs.data());
  });
  ASSERT_EQ(addr_future.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "mesh-enabled machined failed to start (port " << kInternalPort << " in use?)";
  const Address machined_addr = addr_future.get();

  // Gossip a fictitious remote CellApp straight to the machined's mesh port.
  EventDispatcher mesh_disp{"mesh_sender"};
  mesh_disp.SetMaxPollWait(Milliseconds(1));
  MeshTransport sender(mesh_disp);
  ASSERT_TRUE(sender.Open(Address("127.0.0.1", 0), Address("127.0.0.1", 0)).HasValue());

  machined::MeshRegistryMsg gossip;
  gossip.owner = Address("127.0.0.1", 29000);
  ProcessInfo remote;
  remote.process_type = ProcessType::kCellApp;
  remote.name = "remote-cellapp";
  remote.internal_addr = Address("10.0.0.5", 7000);
  remote.pid = 999;
  gossip.processes.push_back(remote);
  BinaryWriter w;
  gossip.Serialize(w);
  ASSERT_TRUE(sender.SendTo(Address("127.0.0.1", kMeshPort), w.Data()).HasValue());
  poll_until(mesh_disp, [&] { return false; }, std::chrono::milliseconds(50));

  EventDispatcher client_disp{"mesh_client"};
  client_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface client_ni(client_disp);
  MachinedClient client(client_disp, client_ni);
  ASSERT_TRUE(client.Connect(machined_addr));
  ASSERT_TRUE(poll_until(client_disp, [&] { return client.IsConnected(); }));

  auto result = wait_for_registry_entry(client_disp, client, ProcessType::kCellApp);

  stop.store(true, std::memory_order_release);
  machined_thread.join();

  ASSERT_EQ(result.size(), 1u) << "gossiped remote CellApp should resolve via the mesh";
  EXPECT_EQ(result[0].name, "remote-cellapp");
  EXPECT_EQ(result[0].internal_addr.Port(), 7000);
}
