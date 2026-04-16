#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dbapp/dbapp.h"
#include "dbapp/dbapp_messages.h"
#include "loginapp/login_messages.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/socket.h"

using namespace atlas;

namespace {

template <typename Pred>
bool poll_until(EventDispatcher& disp, Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

auto reserve_udp_port() -> uint16_t {
  auto sock = Socket::CreateUdp();
  EXPECT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto local = sock->LocalAddress();
  EXPECT_TRUE(local.HasValue());
  return local ? local->Port() : 0;
}

auto write_entity_defs_json() -> std::filesystem::path {
  auto path = std::filesystem::temp_directory_path() / "atlas_dbapp_entity_defs.json";
  std::ofstream f(path, std::ios::trunc);
  f << R"({
        "version": 1,
        "types": [
            {
                "type_id": 1,
                "name": "Account",
                "has_cell": false,
                "has_client": true,
                "properties": [
                    {"name": "accountName", "type": "string", "persistent": true,
                     "identifier": true, "scope": "base_only", "index": 0},
                    {"name": "level", "type": "int32", "persistent": true,
                     "identifier": false, "scope": "base_only", "index": 1}
                ]
            }
        ]
    })";
  return path;
}

struct DBAppArgv {
  DBAppArgv(uint16_t internal_port, const std::filesystem::path& entitydef_path,
            const std::filesystem::path& sqlite_path)
      : storage{"dbapp",
                "--type",
                "dbapp",
                "--name",
                "dbapp_test",
                "--update-hertz",
                "100",
                "--internal-port",
                std::to_string(internal_port),
                "--machined",
                "127.0.0.1:1",
                "--entitydef-path",
                entitydef_path.string(),
                "--db-type",
                "sqlite",
                "--db-sqlite-path",
                sqlite_path.string(),
                "--account-type-id",
                "1",
                "--auto-create-accounts",
                "true"} {
    for (auto& s : storage) ptrs.push_back(s.data());
  }

  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }

  std::vector<std::string> storage;
  std::vector<char*> ptrs;
};

class TestDBApp final : public DBApp {
 public:
  TestDBApp(EventDispatcher& dispatcher, NetworkInterface& network, std::promise<Address>& addr,
            std::atomic<bool>& stop_flag)
      : DBApp(dispatcher, network), addr_promise_(addr), stop_flag_(stop_flag) {}

 protected:
  auto Init(int argc, char* argv[]) -> bool override {
    if (!DBApp::Init(argc, argv)) return false;

    Address addr = Network().RudpAddress();
    if (addr.Ip() == 0) addr = Address("127.0.0.1", addr.Port());
    addr_promise_.set_value(addr);
    return true;
  }

  void OnTickComplete() override {
    DBApp::OnTickComplete();
    if (stop_flag_.load(std::memory_order_acquire)) Shutdown();
  }

 private:
  std::promise<Address>& addr_promise_;
  std::atomic<bool>& stop_flag_;
};

struct DBAppClient {
  explicit DBAppClient(std::string name) : dispatcher(std::move(name)), network(dispatcher) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    (void)network.InterfaceTable().RegisterTypedHandler<login::AuthLoginResult>(
        [this](const Address&, Channel*, const login::AuthLoginResult& msg) {
          auth_result = msg;
          auth_received.store(true, std::memory_order_release);
        });
  }

  void start_local_rudp(uint16_t port) {
    ASSERT_TRUE(network.StartRudpServer(Address("127.0.0.1", port)).HasValue());
  }

  void reset_auth() { auth_received.store(false, std::memory_order_release); }

  EventDispatcher dispatcher;
  NetworkInterface network;
  std::atomic<bool> auth_received{false};
  login::AuthLoginResult auth_result;
};

}  // namespace

TEST(DBAppIntegration, AuthLoginRejectsMissingAccountThenAutoCreatesAndReauths) {
  const uint16_t port = reserve_udp_port();
  ASSERT_NE(port, 0u);

  const auto entity_defs = write_entity_defs_json();
  const auto sqlite_path =
      std::filesystem::temp_directory_path() / "atlas_dbapp_login_flow.sqlite3";
  std::filesystem::remove(sqlite_path);

  std::promise<Address> addr_promise;
  auto addr_future = addr_promise.get_future();
  std::atomic<bool> stop_flag{false};

  std::thread app_thread([&]() {
    EventDispatcher dispatcher{"dbapp_server"};
    dispatcher.SetMaxPollWait(Milliseconds(1));
    NetworkInterface network(dispatcher);
    TestDBApp app(dispatcher, network, addr_promise, stop_flag);
    DBAppArgv args(port, entity_defs, sqlite_path);
    EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 0);
  });

  const Address server_addr = addr_future.get();
  ASSERT_NE(server_addr.Port(), 0u);

  DBAppClient client{"dbapp_login_client"};
  client.start_local_rudp(reserve_udp_port());

  auto ch = client.network.ConnectRudp(server_addr);
  ASSERT_TRUE(ch.HasValue()) << ch.Error().Message();

  login::AuthLogin missing;
  missing.request_id = 1;
  missing.username = "hero";
  missing.password_hash = "pw";
  missing.auto_create = false;
  ASSERT_TRUE((*ch)->SendMessage(missing).HasValue());
  ASSERT_TRUE(poll_until(client.dispatcher,
                         [&] { return client.auth_received.load(std::memory_order_acquire); }));
  EXPECT_FALSE(client.auth_result.success);
  EXPECT_EQ(client.auth_result.status, login::LoginStatus::kInvalidCredentials);

  client.reset_auth();
  login::AuthLogin create = missing;
  create.request_id = 2;
  create.auto_create = true;
  ASSERT_TRUE((*ch)->SendMessage(create).HasValue());
  ASSERT_TRUE(poll_until(client.dispatcher,
                         [&] { return client.auth_received.load(std::memory_order_acquire); }));
  EXPECT_TRUE(client.auth_result.success);
  EXPECT_EQ(client.auth_result.status, login::LoginStatus::kSuccess);
  EXPECT_GT(client.auth_result.dbid, 0);
  EXPECT_EQ(client.auth_result.type_id, 1u);
  const auto created_dbid = client.auth_result.dbid;

  client.reset_auth();
  login::AuthLogin reauth = missing;
  reauth.request_id = 3;
  ASSERT_TRUE((*ch)->SendMessage(reauth).HasValue());
  ASSERT_TRUE(poll_until(client.dispatcher,
                         [&] { return client.auth_received.load(std::memory_order_acquire); }));
  EXPECT_TRUE(client.auth_result.success);
  EXPECT_EQ(client.auth_result.status, login::LoginStatus::kSuccess);
  EXPECT_EQ(client.auth_result.dbid, created_dbid);

  client.reset_auth();
  login::AuthLogin wrong_pw = missing;
  wrong_pw.request_id = 4;
  wrong_pw.password_hash = "wrong_pw";
  ASSERT_TRUE((*ch)->SendMessage(wrong_pw).HasValue());
  ASSERT_TRUE(poll_until(client.dispatcher,
                         [&] { return client.auth_received.load(std::memory_order_acquire); }));
  EXPECT_FALSE(client.auth_result.success);
  EXPECT_EQ(client.auth_result.status, login::LoginStatus::kInvalidCredentials);

  stop_flag.store(true, std::memory_order_release);
  app_thread.join();

  std::filesystem::remove(sqlite_path);
  std::filesystem::remove(entity_defs);
}
