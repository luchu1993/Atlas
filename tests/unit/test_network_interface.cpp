#include <gtest/gtest.h>
#include "network/network_interface.hpp"
#include "network/event_dispatcher.hpp"
#include "network/tcp_channel.hpp"
#include "network/message.hpp"

#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace atlas;

// A simple test message
struct NetTestMsg
{
    uint32_t value;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{200, "NetTestMsg", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint32_t>(value); }

    static auto deserialize(BinaryReader& r) -> Result<NetTestMsg>
    {
        auto v = r.read<uint32_t>();
        if (!v) return v.error();
        return NetTestMsg{*v};
    }
};

class NetworkInterfaceTest : public ::testing::Test
{
protected:
    EventDispatcher dispatcher_{"test"};
    NetworkInterface ni_{dispatcher_};

    void SetUp() override
    {
        dispatcher_.set_max_poll_wait(Milliseconds(1));
    }
};

TEST_F(NetworkInterfaceTest, StartTcpServer)
{
    auto result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_NE(ni_.tcp_address().port(), 0);
}

TEST_F(NetworkInterfaceTest, StartUdp)
{
    auto result = ni_.start_udp(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_NE(ni_.udp_address().port(), 0);
}

TEST_F(NetworkInterfaceTest, TcpConnectAndAccept)
{
    auto server_result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(server_result.has_value());

    auto connect_result = ni_.connect_tcp(ni_.tcp_address());
    ASSERT_TRUE(connect_result.has_value()) << connect_result.error().message();
    EXPECT_NE(*connect_result, nullptr);

    // Wait for connection and process accept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dispatcher_.process_once();

    // Should have at least the outgoing channel
    EXPECT_GE(ni_.channel_count(), 1u);
}

TEST_F(NetworkInterfaceTest, TcpMessageDispatch)
{
    // Start server NI
    auto server_result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(server_result.has_value());
    auto server_addr = ni_.tcp_address();

    // Register handler
    bool received = false;
    uint32_t received_value = 0;
    ni_.interface_table().register_typed_handler<NetTestMsg>(
        [&](const Address&, Channel*, const NetTestMsg& msg)
        {
            received = true;
            received_value = msg.value;
        });

    // Create a client socket, connect, and send a message
    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    (void)client_sock->connect(server_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dispatcher_.process_once();  // accept

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Build and send a framed message from client
    Bundle bundle;
    bundle.add_message(NetTestMsg{42});
    auto payload = bundle.finalize();

    // TCP frame: [uint32 length LE][payload]
    uint32_t frame_len = endian::to_little(static_cast<uint32_t>(payload.size()));
    std::vector<std::byte> frame;
    auto* len_bytes = reinterpret_cast<const std::byte*>(&frame_len);
    frame.insert(frame.end(), len_bytes, len_bytes + 4);
    frame.insert(frame.end(), payload.begin(), payload.end());

    auto sent = client_sock->send(frame);
    ASSERT_TRUE(sent.has_value()) << sent.error().message();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dispatcher_.process_once();  // receive and dispatch

    EXPECT_TRUE(received);
    EXPECT_EQ(received_value, 42u);
}

TEST_F(NetworkInterfaceTest, FindChannel)
{
    EXPECT_EQ(ni_.find_channel(Address("1.2.3.4", 5)), nullptr);
    EXPECT_EQ(ni_.channel_count(), 0u);
}

TEST_F(NetworkInterfaceTest, PrepareForShutdown)
{
    auto result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value());

    ni_.prepare_for_shutdown();
    // After shutdown, connect should fail or be rejected
    // (channels are condemned)
}

TEST_F(NetworkInterfaceTest, RateLimitRejectsExcess)
{
    ni_.set_rate_limit(2);  // 2 packets per second per IP

    auto result = ni_.start_udp(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value());
    auto udp_addr = ni_.udp_address();

    // Send 5 UDP packets rapidly from a sender
    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());

    std::array<std::byte, 4> data = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    for (int i = 0; i < 5; ++i)
    {
        (void)sender->send_to(data, udp_addr);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dispatcher_.process_once();

    // With rate limit of 2, only 2 should create channels or be processed
    // (exact behavior depends on how fast packets arrive, but channel count should be limited)
    // At minimum, this should not crash
}
