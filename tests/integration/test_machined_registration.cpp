// Integration test: MachinedApp full registration / heartbeat / query flow.
//
// Scenario:
//   - MachinedApp runs in a background thread with its own dispatcher + NI.
//   - A "client" NetworkInterface connects via MachinedClient.
//   - Client registers as ProcessType::BaseApp; queries machined for the entry.
//   - Client deregisters; a second query returns zero entries.
//   - Duplicate registration is rejected.
//   - Multiple distinct processes can register simultaneously.

#include "machined/machined_app.hpp"
#include "network/event_dispatcher.hpp"
#include "network/machined_types.hpp"
#include "network/network_interface.hpp"
#include "server/machined_client.hpp"
#include "server/server_config.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace atlas;
using namespace atlas::machined;

// ============================================================================
// Helpers
// ============================================================================

template <typename Pred>
static bool poll_until(EventDispatcher& disp, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        disp.process_once();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Issue repeated queries until the registry returns a non-empty list for
// process_type, or timeout expires.  Returns the last result.
static std::vector<ProcessInfo> wait_for_registry_entry(
    EventDispatcher& disp, MachinedClient& client, ProcessType type,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
{
    std::vector<ProcessInfo> result;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        bool done = false;
        client.query_async(type,
                           [&](std::vector<ProcessInfo> infos)
                           {
                               result = std::move(infos);
                               done = true;
                           });
        poll_until(disp, [&] { return done; }, std::chrono::milliseconds(300));
        if (!result.empty())
            break;
    }
    return result;
}

struct MachinedArgv
{
    std::vector<std::string> storage{"machined", "--type",          "machined",
                                     "--name",   "machined",        "--update-hertz",
                                     "100",      "--internal-port", "0"};
    std::vector<char*> ptrs;

    MachinedArgv()
    {
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }

    int argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// ============================================================================
// TestMachinedApp: wraps MachinedApp with an external stop flag (atomic) and
// publishes the listen address once init() completes.
// ============================================================================

class TestMachinedApp : public MachinedApp
{
public:
    TestMachinedApp(EventDispatcher& d, NetworkInterface& n, std::promise<Address>& addr_promise,
                    std::atomic<bool>& stop_flag)
        : MachinedApp(d, n), addr_promise_(addr_promise), stop_flag_(stop_flag)
    {
    }

protected:
    auto init(int argc, char* argv[]) -> bool override
    {
        if (!MachinedApp::init(argc, argv))
            return false;
        // Publish the listening address before run() starts so the test thread
        // can connect without a race on NetworkInterface.
        // Substitute 0.0.0.0 → 127.0.0.1 so clients can actually connect.
        Address addr = network().tcp_address();
        if (addr.ip() == 0)
            addr = Address("127.0.0.1", addr.port());
        addr_promise_.set_value(addr);
        return true;
    }

    void on_tick_complete() override
    {
        MachinedApp::on_tick_complete();
        if (stop_flag_.load(std::memory_order_acquire))
            shutdown();
    }

private:
    std::promise<Address>& addr_promise_;
    std::atomic<bool>& stop_flag_;
};

// ============================================================================
// Fixture
// ============================================================================

class MachinedRegistrationTest : public ::testing::Test
{
protected:
    // Client side — driven manually in each test via poll_until.
    EventDispatcher client_disp_{"client"};
    NetworkInterface client_ni_{client_disp_};

    Address machined_addr_;

    std::atomic<bool> stop_flag_{false};
    std::thread machined_thread_;

    void SetUp() override
    {
        client_disp_.set_max_poll_wait(Milliseconds(1));

        std::promise<Address> addr_promise;
        auto addr_future = addr_promise.get_future();

        // All machined objects live on the background thread to avoid
        // cross-thread access to non-thread-safe NetworkInterface.
        machined_thread_ = std::thread(
            [promise = std::move(addr_promise), &stop_flag = stop_flag_]() mutable
            {
                EventDispatcher disp("machined");
                NetworkInterface ni(disp);
                TestMachinedApp app(disp, ni, promise, stop_flag);

                MachinedArgv args;
                app.run_app(args.argc(), args.argv());
            });

        auto status = addr_future.wait_for(std::chrono::seconds(10));
        ASSERT_EQ(status, std::future_status::ready) << "MachinedApp failed to start in time";
        machined_addr_ = addr_future.get();
    }

    void TearDown() override
    {
        stop_flag_.store(true, std::memory_order_release);
        if (machined_thread_.joinable())
            machined_thread_.join();
    }
};

// ============================================================================
// Test: Register → query returns the registered entry
// ============================================================================

TEST_F(MachinedRegistrationTest, RegisterReceivesAck)
{
    MachinedClient client(client_disp_, client_ni_);

    ASSERT_TRUE(client.connect(machined_addr_))
        << "MachinedClient::connect failed for " << machined_addr_.to_string();
    ASSERT_TRUE(poll_until(client_disp_, [&] { return client.is_connected(); }))
        << "Client did not connect to MachinedApp";

    ServerConfig cfg;
    cfg.process_type = ProcessType::BaseApp;
    cfg.process_name = "baseapp-1";
    cfg.internal_port = 7200;
    client.send_register(cfg);

    // Poll until the registration appears in machined's registry.
    // Each iteration issues one query and drains responses; we stop once the
    // returned list is non-empty.
    std::vector<ProcessInfo> result;
    bool found = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (!found && std::chrono::steady_clock::now() < deadline)
    {
        bool query_done = false;
        client.query_async(ProcessType::BaseApp,
                           [&](std::vector<ProcessInfo> infos)
                           {
                               result = std::move(infos);
                               query_done = true;
                           });
        // Drain until QueryResponse arrives.
        poll_until(client_disp_, [&] { return query_done; }, std::chrono::milliseconds(200));
        if (!result.empty())
            found = true;
    }

    ASSERT_TRUE(found) << "Registration never appeared in machined registry";
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].process_type, ProcessType::BaseApp);
    EXPECT_EQ(result[0].name, "baseapp-1");
    EXPECT_EQ(result[0].internal_addr.port(), 7200);
}

// ============================================================================
// Test: Heartbeat round-trip (register first, then heartbeat)
// ============================================================================

TEST_F(MachinedRegistrationTest, HeartbeatReceivesAck)
{
    MachinedClient client(client_disp_, client_ni_);
    ASSERT_TRUE(client.connect(machined_addr_));
    ASSERT_TRUE(poll_until(client_disp_, [&] { return client.is_connected(); }));

    ServerConfig cfg;
    cfg.process_type = ProcessType::BaseApp;
    cfg.process_name = "baseapp-hb";
    cfg.internal_port = 7201;
    client.send_register(cfg);

    auto reg_result = wait_for_registry_entry(client_disp_, client, ProcessType::BaseApp);
    ASSERT_FALSE(reg_result.empty()) << "Registration did not complete before heartbeat test";

    client.send_heartbeat(0.3f, 10);
    poll_until(client_disp_, [&] { return false; }, std::chrono::milliseconds(100));
}

// ============================================================================
// Test: Deregister removes entry from registry
// ============================================================================

TEST_F(MachinedRegistrationTest, DeregisterRemovesEntry)
{
    MachinedClient client(client_disp_, client_ni_);
    ASSERT_TRUE(client.connect(machined_addr_));
    ASSERT_TRUE(poll_until(client_disp_, [&] { return client.is_connected(); }));

    ServerConfig cfg;
    cfg.process_type = ProcessType::BaseApp;
    cfg.process_name = "baseapp-dereg";
    cfg.internal_port = 7202;
    client.send_register(cfg);

    auto reg_result = wait_for_registry_entry(client_disp_, client, ProcessType::BaseApp);
    ASSERT_FALSE(reg_result.empty()) << "Registration must succeed before deregister test";

    client.send_deregister(cfg);

    // Give machined a moment to process the deregister, then query once.
    poll_until(client_disp_, [&] { return false; }, std::chrono::milliseconds(100));

    std::vector<ProcessInfo> result;
    bool query_done = false;
    client.query_async(ProcessType::BaseApp,
                       [&](std::vector<ProcessInfo> infos)
                       {
                           result = std::move(infos);
                           query_done = true;
                       });
    ASSERT_TRUE(poll_until(client_disp_, [&] { return query_done; }));
    EXPECT_TRUE(result.empty()) << "Registry should be empty after deregister";
}

// ============================================================================
// Test: Duplicate registration is rejected
// ============================================================================

TEST_F(MachinedRegistrationTest, DuplicateRegistrationRejected)
{
    MachinedClient client_a(client_disp_, client_ni_);
    ASSERT_TRUE(client_a.connect(machined_addr_));
    ASSERT_TRUE(poll_until(client_disp_, [&] { return client_a.is_connected(); }));

    ServerConfig cfg;
    cfg.process_type = ProcessType::BaseApp;
    cfg.process_name = "baseapp-dup";
    cfg.internal_port = 7203;
    client_a.send_register(cfg);

    auto reg1 = wait_for_registry_entry(client_disp_, client_a, ProcessType::BaseApp);
    ASSERT_FALSE(reg1.empty()) << "First registration must succeed";

    // Second client with the same (type, name) — should be rejected.
    EventDispatcher client2_disp{"client2"};
    client2_disp.set_max_poll_wait(Milliseconds(1));
    NetworkInterface client2_ni(client2_disp);

    MachinedClient client_b(client2_disp, client2_ni);
    ASSERT_TRUE(client_b.connect(machined_addr_));
    ASSERT_TRUE(poll_until(client2_disp, [&] { return client_b.is_connected(); }));
    client_b.send_register(cfg);

    // Drain client2 to let machined process the duplicate register.
    poll_until(client2_disp, [&] { return false; }, std::chrono::milliseconds(200));

    // Query from client_a — still exactly one entry.
    std::vector<ProcessInfo> result;
    bool query_done = false;
    client_a.query_async(ProcessType::BaseApp,
                         [&](std::vector<ProcessInfo> infos)
                         {
                             result = std::move(infos);
                             query_done = true;
                         });
    ASSERT_TRUE(poll_until(client_disp_, [&] { return query_done; }));
    ASSERT_EQ(result.size(), 1u) << "Only one entry expected after duplicate rejection";
    EXPECT_EQ(result[0].name, "baseapp-dup");
}

// ============================================================================
// Test: Multiple distinct processes can register simultaneously
// ============================================================================

TEST_F(MachinedRegistrationTest, MultipleProcessesCanRegister)
{
    MachinedClient client1(client_disp_, client_ni_);
    ASSERT_TRUE(client1.connect(machined_addr_));
    ASSERT_TRUE(poll_until(client_disp_, [&] { return client1.is_connected(); }));

    EventDispatcher client2_disp{"client2"};
    client2_disp.set_max_poll_wait(Milliseconds(1));
    NetworkInterface client2_ni(client2_disp);
    MachinedClient client2(client2_disp, client2_ni);
    ASSERT_TRUE(client2.connect(machined_addr_));
    ASSERT_TRUE(poll_until(client2_disp, [&] { return client2.is_connected(); }));

    ServerConfig cfg1;
    cfg1.process_type = ProcessType::BaseApp;
    cfg1.process_name = "baseapp-multi-1";
    cfg1.internal_port = 7210;
    client1.send_register(cfg1);

    ServerConfig cfg2;
    cfg2.process_type = ProcessType::BaseApp;
    cfg2.process_name = "baseapp-multi-2";
    cfg2.internal_port = 7211;
    client2.send_register(cfg2);

    // Wait for both entries to appear via client1's query.
    std::vector<ProcessInfo> result;
    auto deadline2 = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (result.size() < 2 && std::chrono::steady_clock::now() < deadline2)
    {
        // Drain client2 so its RegisterMessage reaches machined.
        client2_disp.process_once();

        bool done = false;
        client1.query_async(ProcessType::BaseApp,
                            [&](std::vector<ProcessInfo> infos)
                            {
                                result = std::move(infos);
                                done = true;
                            });
        poll_until(client_disp_, [&] { return done; }, std::chrono::milliseconds(300));
    }
    EXPECT_EQ(result.size(), 2u) << "Both BaseApp processes should be registered";
}
