#include "baseappmgr/baseappmgr.hpp"
#include "baseappmgr/baseappmgr_messages.hpp"
#include "loginapp/login_messages.hpp"
#include "network/event_dispatcher.hpp"
#include "network/network_interface.hpp"
#include "network/reliable_udp.hpp"
#include "network/socket.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace atlas;
using namespace atlas::baseappmgr;

namespace
{

template <typename Pred>
bool poll_until(EventDispatcher& disp, Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
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

struct BaseAppMgrArgv
{
    explicit BaseAppMgrArgv(uint16_t internal_port)
        : storage{"baseappmgr", "--type",          "baseappmgr",
                  "--name",     "baseappmgr_test", "--update-hertz",
                  "100",        "--internal-port", std::to_string(internal_port),
                  "--machined", "127.0.0.1:1"}
    {
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }

    int argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }

    std::vector<std::string> storage;
    std::vector<char*> ptrs;
};

class TestBaseAppMgr final : public BaseAppMgr
{
public:
    TestBaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network,
                   std::promise<Address>& addr_promise, std::atomic<bool>& stop_flag)
        : BaseAppMgr(dispatcher, network), addr_promise_(addr_promise), stop_flag_(stop_flag)
    {
    }

protected:
    auto init(int argc, char* argv[]) -> bool override
    {
        if (!BaseAppMgr::init(argc, argv))
            return false;

        Address addr = network().rudp_address();
        if (addr.ip() == 0)
            addr = Address("127.0.0.1", addr.port());
        addr_promise_.set_value(addr);
        return true;
    }

    void on_tick_complete() override
    {
        if (stop_flag_.load(std::memory_order_acquire))
            shutdown();
    }

private:
    std::promise<Address>& addr_promise_;
    std::atomic<bool>& stop_flag_;
};

struct BaseAppMgrClient
{
    explicit BaseAppMgrClient(std::string name) : dispatcher(std::move(name)), network(dispatcher)
    {
        dispatcher.set_max_poll_wait(Milliseconds(1));
        network.interface_table().register_typed_handler<RegisterBaseAppAck>(
            [this](const Address&, Channel*, const RegisterBaseAppAck& msg)
            {
                register_ack = msg;
                register_ack_received.store(true, std::memory_order_release);
            });
        network.interface_table().register_typed_handler<RequestEntityIdRangeAck>(
            [this](const Address&, Channel*, const RequestEntityIdRangeAck& msg)
            {
                id_range_ack = msg;
                id_range_ack_received.store(true, std::memory_order_release);
            });
        network.interface_table().register_typed_handler<login::AllocateBaseAppResult>(
            [this](const Address&, Channel*, const login::AllocateBaseAppResult& msg)
            {
                allocate_result = msg;
                allocate_result_received.store(true, std::memory_order_release);
            });
    }

    void start_local_rudp(uint16_t port)
    {
        ASSERT_TRUE(network.start_rudp_server(Address("127.0.0.1", port)).has_value());
    }

    void reset_id_range_ack() { id_range_ack_received.store(false, std::memory_order_release); }
    void reset_allocate_result()
    {
        allocate_result_received.store(false, std::memory_order_release);
    }

    EventDispatcher dispatcher;
    NetworkInterface network;
    std::atomic<bool> register_ack_received{false};
    std::atomic<bool> id_range_ack_received{false};
    std::atomic<bool> allocate_result_received{false};
    RegisterBaseAppAck register_ack;
    RequestEntityIdRangeAck id_range_ack;
    login::AllocateBaseAppResult allocate_result;
};

}  // namespace

TEST(BaseAppMgrIntegration, RejectsDuplicateInternalAddressWithoutConsumingAppId)
{
    const uint16_t port = reserve_udp_port();
    ASSERT_NE(port, 0u);

    std::promise<Address> addr_promise;
    auto addr_future = addr_promise.get_future();
    std::atomic<bool> stop_flag{false};

    std::thread app_thread(
        [&]()
        {
            EventDispatcher dispatcher{"baseappmgr_server"};
            dispatcher.set_max_poll_wait(Milliseconds(1));
            NetworkInterface network(dispatcher);
            TestBaseAppMgr app(dispatcher, network, addr_promise, stop_flag);
            BaseAppMgrArgv args(port);
            EXPECT_EQ(app.run_app(args.argc(), args.argv()), 0);
        });

    const Address server_addr = addr_future.get();
    ASSERT_NE(server_addr.port(), 0u);

    BaseAppMgrClient client1{"baseappmgr_client1"};
    BaseAppMgrClient client2{"baseappmgr_client2"};
    BaseAppMgrClient client3{"baseappmgr_client3"};

    auto ch1 = client1.network.connect_rudp(server_addr);
    auto ch2 = client2.network.connect_rudp(server_addr);
    auto ch3 = client3.network.connect_rudp(server_addr);
    ASSERT_TRUE(ch1.has_value()) << ch1.error().message();
    ASSERT_TRUE(ch2.has_value()) << ch2.error().message();
    ASSERT_TRUE(ch3.has_value()) << ch3.error().message();

    RegisterBaseApp reg1;
    reg1.internal_addr = Address(0, 7101);
    reg1.external_addr = Address(0, 17101);
    ASSERT_TRUE((*ch1)->send_message(reg1).has_value());
    ASSERT_TRUE(
        poll_until(client1.dispatcher,
                   [&] { return client1.register_ack_received.load(std::memory_order_acquire); }))
        << "first BaseApp registration ack not received";
    EXPECT_TRUE(client1.register_ack.success);
    EXPECT_EQ(client1.register_ack.app_id, 1u);

    RegisterBaseApp reg2;
    reg2.internal_addr = Address(0, 7101);
    reg2.external_addr = Address(0, 17102);
    ASSERT_TRUE((*ch2)->send_message(reg2).has_value());
    ASSERT_TRUE(
        poll_until(client2.dispatcher,
                   [&] { return client2.register_ack_received.load(std::memory_order_acquire); }))
        << "duplicate BaseApp registration ack not received";
    EXPECT_FALSE(client2.register_ack.success);
    EXPECT_EQ(client2.register_ack.app_id, 0u);

    RegisterBaseApp reg3;
    reg3.internal_addr = Address(0, 7102);
    reg3.external_addr = Address(0, 17103);
    ASSERT_TRUE((*ch3)->send_message(reg3).has_value());
    ASSERT_TRUE(
        poll_until(client3.dispatcher,
                   [&] { return client3.register_ack_received.load(std::memory_order_acquire); }))
        << "third BaseApp registration ack not received";
    EXPECT_TRUE(client3.register_ack.success);
    EXPECT_EQ(client3.register_ack.app_id, 2u);
    EXPECT_EQ(client3.register_ack.entity_id_start, client1.register_ack.entity_id_end + 1);

    stop_flag.store(true, std::memory_order_release);
    app_thread.join();
}

TEST(BaseAppMgrIntegration, IgnoresSpoofedFollowUpMessagesFromWrongBaseApp)
{
    const uint16_t port = reserve_udp_port();
    ASSERT_NE(port, 0u);

    std::promise<Address> addr_promise;
    auto addr_future = addr_promise.get_future();
    std::atomic<bool> stop_flag{false};

    std::thread app_thread(
        [&]()
        {
            EventDispatcher dispatcher{"baseappmgr_server"};
            dispatcher.set_max_poll_wait(Milliseconds(1));
            NetworkInterface network(dispatcher);
            TestBaseAppMgr app(dispatcher, network, addr_promise, stop_flag);
            BaseAppMgrArgv args(port);
            EXPECT_EQ(app.run_app(args.argc(), args.argv()), 0);
        });

    const Address server_addr = addr_future.get();
    ASSERT_NE(server_addr.port(), 0u);

    BaseAppMgrClient baseapp1{"baseappmgr_baseapp1"};
    BaseAppMgrClient baseapp2{"baseappmgr_baseapp2"};
    BaseAppMgrClient login_client{"baseappmgr_login"};
    baseapp1.start_local_rudp(7101);
    baseapp2.start_local_rudp(7102);

    auto ch1 = baseapp1.network.connect_rudp(server_addr);
    auto ch2 = baseapp2.network.connect_rudp(server_addr);
    auto login_ch = login_client.network.connect_rudp(server_addr);
    ASSERT_TRUE(ch1.has_value()) << ch1.error().message();
    ASSERT_TRUE(ch2.has_value()) << ch2.error().message();
    ASSERT_TRUE(login_ch.has_value()) << login_ch.error().message();

    RegisterBaseApp reg1;
    reg1.internal_addr = Address(0, 7101);
    reg1.external_addr = Address(0, 17101);
    ASSERT_TRUE((*ch1)->send_message(reg1).has_value());
    ASSERT_TRUE(
        poll_until(baseapp1.dispatcher,
                   [&] { return baseapp1.register_ack_received.load(std::memory_order_acquire); }));

    RegisterBaseApp reg2;
    reg2.internal_addr = Address(0, 7102);
    reg2.external_addr = Address(0, 17102);
    ASSERT_TRUE((*ch2)->send_message(reg2).has_value());
    ASSERT_TRUE(
        poll_until(baseapp2.dispatcher,
                   [&] { return baseapp2.register_ack_received.load(std::memory_order_acquire); }));

    ASSERT_TRUE(baseapp1.register_ack.success);
    ASSERT_TRUE(baseapp2.register_ack.success);
    EXPECT_EQ(baseapp1.register_ack.app_id, 1u);
    EXPECT_EQ(baseapp2.register_ack.app_id, 2u);

    BaseAppReady spoof_ready;
    spoof_ready.app_id = baseapp1.register_ack.app_id;
    ASSERT_TRUE((*ch2)->send_message(spoof_ready).has_value());

    BaseAppReady ready2;
    ready2.app_id = baseapp2.register_ack.app_id;
    ASSERT_TRUE((*ch2)->send_message(ready2).has_value());

    InformLoad spoof_load;
    spoof_load.app_id = baseapp1.register_ack.app_id;
    spoof_load.load = 0.0f;
    spoof_load.entity_count = 1;
    spoof_load.proxy_count = 1;
    ASSERT_TRUE((*ch2)->send_message(spoof_load).has_value());

    InformLoad load2;
    load2.app_id = baseapp2.register_ack.app_id;
    load2.load = 0.8f;
    load2.entity_count = 10;
    load2.proxy_count = 5;
    ASSERT_TRUE((*ch2)->send_message(load2).has_value());

    login::AllocateBaseApp alloc;
    alloc.request_id = 77;
    alloc.type_id = 1;
    alloc.dbid = 123;
    ASSERT_TRUE((*login_ch)->send_message(alloc).has_value());
    ASSERT_TRUE(poll_until(
        login_client.dispatcher,
        [&] { return login_client.allocate_result_received.load(std::memory_order_acquire); }));

    EXPECT_TRUE(login_client.allocate_result.success);
    EXPECT_EQ(login_client.allocate_result.request_id, 77u);
    EXPECT_EQ(login_client.allocate_result.internal_addr.port(), 7102u);
    EXPECT_EQ(login_client.allocate_result.external_addr.port(), 17102u);

    baseapp2.reset_id_range_ack();
    RequestEntityIdRange spoof_range;
    spoof_range.app_id = baseapp1.register_ack.app_id;
    ASSERT_TRUE((*ch2)->send_message(spoof_range).has_value());
    EXPECT_FALSE(poll_until(
        baseapp2.dispatcher,
        [&] { return baseapp2.id_range_ack_received.load(std::memory_order_acquire); },
        std::chrono::milliseconds(200)));

    RequestEntityIdRange valid_range;
    valid_range.app_id = baseapp2.register_ack.app_id;
    ASSERT_TRUE((*ch2)->send_message(valid_range).has_value());
    ASSERT_TRUE(
        poll_until(baseapp2.dispatcher,
                   [&] { return baseapp2.id_range_ack_received.load(std::memory_order_acquire); }));

    EXPECT_EQ(baseapp2.id_range_ack.app_id, baseapp2.register_ack.app_id);
    EXPECT_EQ(baseapp2.id_range_ack.entity_id_start, baseapp2.register_ack.entity_id_end + 1);

    stop_flag.store(true, std::memory_order_release);
    app_thread.join();
}
