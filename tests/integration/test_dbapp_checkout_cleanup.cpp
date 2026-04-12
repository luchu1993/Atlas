#include "dbapp/dbapp.hpp"
#include "dbapp/dbapp_messages.hpp"
#include "network/event_dispatcher.hpp"
#include "network/machined_types.hpp"
#include "network/network_interface.hpp"
#include "network/reliable_udp.hpp"
#include "network/socket.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

using namespace atlas;
using namespace atlas::dbapp;

namespace
{

template <typename Pred>
bool poll_until(EventDispatcher& disp, Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        disp.process_once();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

auto reserve_udp_port() -> uint16_t
{
    auto sock = Socket::create_udp();
    EXPECT_TRUE(sock.has_value());
    EXPECT_TRUE(sock->bind(Address("127.0.0.1", 0)).has_value());
    auto local = sock->local_address();
    EXPECT_TRUE(local.has_value());
    return local ? local->port() : 0;
}

auto write_entity_defs_json() -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "atlas_dbapp_checkout_entity_defs.json";
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

struct DBAppArgv
{
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
                  "true"}
    {
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }

    int argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }

    std::vector<std::string> storage;
    std::vector<char*> ptrs;
};

class TestDBApp final : public DBApp
{
public:
    TestDBApp(EventDispatcher& dispatcher, NetworkInterface& network, std::promise<Address>& addr,
              std::atomic<bool>& stop_flag)
        : DBApp(dispatcher, network), addr_promise_(addr), stop_flag_(stop_flag)
    {
    }

protected:
    auto init(int argc, char* argv[]) -> bool override
    {
        if (!DBApp::init(argc, argv))
            return false;

        Address addr = network().rudp_address();
        if (addr.ip() == 0)
            addr = Address("127.0.0.1", addr.port());
        addr_promise_.set_value(addr);
        return true;
    }

    void on_tick_complete() override
    {
        DBApp::on_tick_complete();
        if (stop_flag_.load(std::memory_order_acquire))
            shutdown();
    }

private:
    std::promise<Address>& addr_promise_;
    std::atomic<bool>& stop_flag_;
};

struct DBAppClient
{
    explicit DBAppClient(std::string name) : dispatcher(std::move(name)), network(dispatcher)
    {
        dispatcher.set_max_poll_wait(Milliseconds(1));
        (void)network.interface_table().register_typed_handler<WriteEntityAck>(
            [this](const Address&, Channel*, const WriteEntityAck& msg)
            {
                write_ack = msg;
                write_received.store(true, std::memory_order_release);
            });
        (void)network.interface_table().register_typed_handler<CheckoutEntityAck>(
            [this](const Address&, Channel*, const CheckoutEntityAck& msg)
            {
                checkout_ack = msg;
                checkout_received.store(true, std::memory_order_release);
            });
    }

    void start_local_rudp(uint16_t port)
    {
        ASSERT_TRUE(network.start_rudp_server(Address("127.0.0.1", port)).has_value());
    }

    [[nodiscard]] auto local_addr() const -> Address { return network.rudp_address(); }
    void reset_write() { write_received.store(false, std::memory_order_release); }
    void reset_checkout() { checkout_received.store(false, std::memory_order_release); }

    EventDispatcher dispatcher;
    NetworkInterface network;
    std::atomic<bool> write_received{false};
    std::atomic<bool> checkout_received{false};
    WriteEntityAck write_ack;
    CheckoutEntityAck checkout_ack;
};

auto make_blob(std::string_view text) -> std::vector<std::byte>
{
    std::vector<std::byte> blob;
    for (char ch : text)
        blob.push_back(static_cast<std::byte>(ch));
    return blob;
}

}  // namespace

TEST(DBAppIntegration, BaseAppDeathNotificationClearsCheckoutOwnership)
{
    const uint16_t port = reserve_udp_port();
    ASSERT_NE(port, 0u);

    const auto entity_defs = write_entity_defs_json();
    const auto sqlite_path =
        std::filesystem::temp_directory_path() / "atlas_dbapp_checkout_cleanup.sqlite3";
    std::filesystem::remove(sqlite_path);

    std::promise<Address> addr_promise;
    auto addr_future = addr_promise.get_future();
    std::atomic<bool> stop_flag{false};

    std::thread app_thread(
        [&]()
        {
            EventDispatcher dispatcher{"dbapp_server"};
            dispatcher.set_max_poll_wait(Milliseconds(1));
            NetworkInterface network(dispatcher);
            TestDBApp app(dispatcher, network, addr_promise, stop_flag);
            DBAppArgv args(port, entity_defs, sqlite_path);
            EXPECT_EQ(app.run_app(args.argc(), args.argv()), 0);
        });

    const Address server_addr = addr_future.get();
    ASSERT_NE(server_addr.port(), 0u);

    DBAppClient baseapp1{"dbapp_baseapp1"};
    DBAppClient baseapp2{"dbapp_baseapp2"};
    DBAppClient machined_client{"dbapp_fake_machined"};
    baseapp1.start_local_rudp(reserve_udp_port());
    baseapp2.start_local_rudp(reserve_udp_port());
    machined_client.start_local_rudp(reserve_udp_port());

    auto ch1 = baseapp1.network.connect_rudp(server_addr);
    auto ch2 = baseapp2.network.connect_rudp(server_addr);
    auto machined_ch = machined_client.network.connect_rudp(server_addr);
    ASSERT_TRUE(ch1.has_value()) << ch1.error().message();
    ASSERT_TRUE(ch2.has_value()) << ch2.error().message();
    ASSERT_TRUE(machined_ch.has_value()) << machined_ch.error().message();

    WriteEntity write;
    write.flags = WriteFlags::CreateNew;
    write.type_id = 1;
    write.entity_id = 101;
    write.request_id = 1;
    write.identifier = "cleanup_target";
    write.blob = make_blob("entity");
    ASSERT_TRUE((*ch1)->send_message(write).has_value());
    ASSERT_TRUE(poll_until(baseapp1.dispatcher, [&]
                           { return baseapp1.write_received.load(std::memory_order_acquire); }));
    ASSERT_TRUE(baseapp1.write_ack.success);
    const auto dbid = baseapp1.write_ack.dbid;
    ASSERT_GT(dbid, 0);

    CheckoutEntity co1;
    co1.mode = LoadMode::ByDBID;
    co1.type_id = 1;
    co1.dbid = dbid;
    co1.entity_id = 1001;
    co1.request_id = 11;
    co1.owner_addr = baseapp1.local_addr();
    ASSERT_TRUE((*ch1)->send_message(co1).has_value());
    ASSERT_TRUE(poll_until(baseapp1.dispatcher, [&]
                           { return baseapp1.checkout_received.load(std::memory_order_acquire); }));
    ASSERT_EQ(baseapp1.checkout_ack.status, CheckoutStatus::Success);

    machined::DeathNotification death;
    death.process_type = ProcessType::BaseApp;
    death.name = "dead_baseapp";
    death.internal_addr = baseapp1.local_addr();
    death.reason = 1;
    ASSERT_TRUE((*machined_ch)->send_message(death).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CheckoutEntity co2;
    co2.mode = LoadMode::ByDBID;
    co2.type_id = 1;
    co2.dbid = dbid;
    co2.entity_id = 2002;
    co2.request_id = 12;
    co2.owner_addr = baseapp2.local_addr();
    ASSERT_TRUE((*ch2)->send_message(co2).has_value());
    ASSERT_TRUE(poll_until(baseapp2.dispatcher, [&]
                           { return baseapp2.checkout_received.load(std::memory_order_acquire); }));
    EXPECT_EQ(baseapp2.checkout_ack.status, CheckoutStatus::Success);
    EXPECT_EQ(baseapp2.checkout_ack.dbid, dbid);

    stop_flag.store(true, std::memory_order_release);
    app_thread.join();

    std::filesystem::remove(sqlite_path);
    std::filesystem::remove(entity_defs);
}
