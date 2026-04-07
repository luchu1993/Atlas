#include "network/event_dispatcher.hpp"
#include "network/message.hpp"
#include "network/network_interface.hpp"
#include "network/tcp_channel.hpp"

#include <gtest/gtest.h>

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
        if (!v)
            return v.error();
        return NetTestMsg{*v};
    }
};

// Poll dispatcher until predicate returns true or timeout expires.
// Avoids flaky sleep_for patterns.
template <typename Pred>
static bool poll_until(EventDispatcher& dispatcher, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        dispatcher.process_once();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

class NetworkInterfaceTest : public ::testing::Test
{
protected:
    EventDispatcher dispatcher_{"test"};
    NetworkInterface ni_{dispatcher_};

    void SetUp() override { dispatcher_.set_max_poll_wait(Milliseconds(1)); }
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

    // Poll until the accept event is processed (≥1 channel)
    EXPECT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));
}

TEST_F(NetworkInterfaceTest, TcpMessageDispatch)
{
    // Start server NI
    auto server_result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(server_result.has_value());
    auto server_addr = ni_.tcp_address();

    // Register handler
    std::atomic<bool> received{false};
    std::atomic<uint32_t> received_value{0};
    ni_.interface_table().register_typed_handler<NetTestMsg>(
        [&](const Address&, Channel*, const NetTestMsg& msg)
        {
            received_value.store(msg.value, std::memory_order_relaxed);
            received.store(true, std::memory_order_release);
        });

    // Create a client socket and connect
    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    (void)client_sock->connect(server_addr);

    // Poll until accept event processed (channel appears on server side)
    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

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

    // Poll until the message handler fires
    ASSERT_TRUE(poll_until(dispatcher_, [&] { return received.load(std::memory_order_acquire); }));

    EXPECT_EQ(received_value.load(), 42u);
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
    ni_.set_rate_limit(2);  // max 2 packets per second per IP

    auto result = ni_.start_udp(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value());
    auto udp_addr = ni_.udp_address();

    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());

    // Burst 10 packets instantly — rate limiter should cap accepted channels at 2
    std::array<std::byte, 4> data{};
    for (int i = 0; i < 10; ++i)
    {
        (void)sender->send_to(data, udp_addr);
    }

    // Drain the receive buffer
    poll_until(dispatcher_, [&] { return false; }, std::chrono::milliseconds(50));

    // channel_count reflects accepted UDP "sessions" — must not exceed rate limit
    EXPECT_LE(ni_.channel_count(), 2u);
}

// ============================================================================
// Shutdown: connect_tcp must fail after prepare_for_shutdown
// ============================================================================

TEST_F(NetworkInterfaceTest, ConnectAfterShutdownReturnsError)
{
    auto result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value());

    ni_.prepare_for_shutdown();

    auto conn = ni_.connect_tcp(ni_.tcp_address());
    EXPECT_FALSE(conn.has_value());
}

// ============================================================================
// Channel lifecycle: condemn removes channel, count returns to 0
// ============================================================================

TEST_F(NetworkInterfaceTest, ChannelCountAfterDisconnect)
{
    auto server_result = ni_.start_tcp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(server_result.has_value());

    auto conn = ni_.connect_tcp(ni_.tcp_address());
    ASSERT_TRUE(conn.has_value());

    // Wait for accept
    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

    // Calling prepare_for_shutdown condemns all channels
    ni_.prepare_for_shutdown();

    // After draining, condemned channels should be cleaned up
    poll_until(dispatcher_, [&] { return false; }, std::chrono::milliseconds(100));

    EXPECT_EQ(ni_.channel_count(), 0u);
}
