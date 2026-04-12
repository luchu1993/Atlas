#include "network/event_dispatcher.hpp"
#include "network/message.hpp"
#include "network/network_interface.hpp"
#include "network/reliable_udp.hpp"
#include "network/tcp_channel.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
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

TEST_F(NetworkInterfaceTest, TcpReadableCallbackDrainsAllBufferedFrames)
{
    ASSERT_TRUE(ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    constexpr uint32_t kFrameCount = 160;
    std::atomic<uint32_t> received_count{0};
    std::atomic<uint32_t> last_value{0};

    ni_.interface_table().register_typed_handler<NetTestMsg>(
        [&](const Address&, Channel*, const NetTestMsg& msg)
        {
            last_value.store(msg.value, std::memory_order_relaxed);
            received_count.fetch_add(1, std::memory_order_relaxed);
        });

    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    auto connect_result = client_sock->connect(ni_.tcp_address());
    ASSERT_TRUE(connect_result.has_value() ||
                connect_result.error().code() == ErrorCode::WouldBlock);

    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

    std::vector<std::byte> frames;
    frames.reserve(kFrameCount * 16);

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        Bundle bundle;
        bundle.add_message(NetTestMsg{i});
        auto payload = bundle.finalize();

        uint32_t frame_len = endian::to_little(static_cast<uint32_t>(payload.size()));
        auto* len_bytes = reinterpret_cast<const std::byte*>(&frame_len);
        frames.insert(frames.end(), len_bytes, len_bytes + sizeof(frame_len));
        frames.insert(frames.end(), payload.begin(), payload.end());
    }

    std::size_t sent_total = 0;
    while (sent_total < frames.size())
    {
        auto sent = client_sock->send(
            std::span<const std::byte>(frames.data() + sent_total, frames.size() - sent_total));
        ASSERT_TRUE(sent.has_value()) << sent.error().message();
        ASSERT_GT(*sent, 0u);
        sent_total += *sent;
    }

    ASSERT_TRUE(poll_until(
        dispatcher_, [&] { return received_count.load(std::memory_order_relaxed) == kFrameCount; },
        std::chrono::milliseconds(1000)));

    EXPECT_EQ(received_count.load(std::memory_order_relaxed), kFrameCount);
    EXPECT_EQ(last_value.load(std::memory_order_relaxed), kFrameCount - 1);
}

TEST_F(NetworkInterfaceTest, FindChannel)
{
    EXPECT_EQ(ni_.find_channel(Address("1.2.3.4", 5)), nullptr);
    EXPECT_EQ(ni_.channel_count(), 0u);
}

TEST_F(NetworkInterfaceTest, FindChannelById)
{
    ASSERT_TRUE(ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    auto connect_result = client_sock->connect(ni_.tcp_address());
    ASSERT_TRUE(connect_result.has_value() ||
                connect_result.error().code() == ErrorCode::WouldBlock);

    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

    auto client_addr = client_sock->local_address();
    ASSERT_TRUE(client_addr.has_value()) << client_addr.error().message();

    auto* channel = ni_.find_channel(*client_addr);
    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(ni_.find_channel(channel->channel_id()), channel);
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

TEST_F(NetworkInterfaceTest, UdpRateLimitedDatagramsAlsoConsumePerPollBudget)
{
    ASSERT_TRUE(ni_.start_udp(Address("127.0.0.1", 0)).has_value());

    std::atomic<uint32_t> received_count{0};
    ni_.interface_table().register_typed_handler<NetTestMsg>(
        [&](const Address&, Channel*, const NetTestMsg&)
        { received_count.fetch_add(1, std::memory_order_relaxed); });

    ni_.set_rate_limit(1);

    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(sender->bind(Address("127.0.0.1", 0)).has_value());

    constexpr uint32_t kDatagramCount = 1500;  // intentionally above current per-poll budget
    for (uint32_t i = 0; i < kDatagramCount; ++i)
    {
        Bundle bundle;
        bundle.add_message(NetTestMsg{i});
        auto payload = bundle.finalize();

        auto sent = sender->send_to(payload, ni_.udp_address());
        ASSERT_TRUE(sent.has_value()) << sent.error().message();
        ASSERT_EQ(*sent, payload.size());
    }

    // A single process_once should not drain the whole UDP socket under flood;
    // only one packet is admitted by the rate limiter and the rest should be
    // left for future polls once the callback budget is exhausted.
    dispatcher_.process_once();
    EXPECT_EQ(received_count.load(std::memory_order_relaxed), 1u);

    ni_.set_rate_limit(0);

    ASSERT_TRUE(poll_until(
        dispatcher_, [&] { return received_count.load(std::memory_order_relaxed) > 1u; },
        std::chrono::milliseconds(500)));
}

TEST_F(NetworkInterfaceTest, TcpRateLimitedAcceptsAlsoConsumePerPollBudget)
{
    ASSERT_TRUE(ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    ni_.set_rate_limit(1);

    constexpr std::size_t kClientCount = 192;  // intentionally above the current accept budget
    std::vector<Socket> clients;
    clients.reserve(kClientCount);

    for (std::size_t i = 0; i < kClientCount; ++i)
    {
        auto sock = Socket::create_tcp();
        ASSERT_TRUE(sock.has_value()) << sock.error().message();

        auto connect_result = sock->connect(ni_.tcp_address());
        ASSERT_TRUE(connect_result.has_value() ||
                    connect_result.error().code() == ErrorCode::WouldBlock ||
                    connect_result.error().code() == ErrorCode::ConnectionRefused)
            << connect_result.error().message();

        clients.push_back(std::move(*sock));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    dispatcher_.process_once();
    EXPECT_EQ(ni_.channel_count(), 1u);

    ni_.set_rate_limit(0);

    ASSERT_TRUE(poll_until(
        dispatcher_, [&] { return ni_.channel_count() > 1u; }, std::chrono::milliseconds(500)));
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

TEST_F(NetworkInterfaceTest, DisconnectCallbackRunsAfterTcpProtocolDisconnect)
{
    ASSERT_TRUE(ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    std::atomic<bool> callback_seen{false};
    std::atomic<ChannelId> callback_channel_id{kInvalidChannelId};
    std::atomic<int> callback_state{-1};
    std::atomic<bool> callback_removed_from_active_set{false};

    ni_.set_disconnect_callback(
        [&](Channel& ch)
        {
            callback_channel_id.store(ch.channel_id(), std::memory_order_relaxed);
            callback_state.store(static_cast<int>(ch.state()), std::memory_order_relaxed);
            callback_removed_from_active_set.store(
                ni_.find_channel(ch.remote_address()) == nullptr &&
                    ni_.find_channel(ch.channel_id()) == nullptr && ni_.channel_count() == 0u,
                std::memory_order_relaxed);
            callback_seen.store(true, std::memory_order_release);
        });

    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    auto connect_result = client_sock->connect(ni_.tcp_address());
    ASSERT_TRUE(connect_result.has_value() ||
                connect_result.error().code() == ErrorCode::WouldBlock);

    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

    auto client_addr = client_sock->local_address();
    ASSERT_TRUE(client_addr.has_value()) << client_addr.error().message();

    auto* channel = ni_.find_channel(*client_addr);
    ASSERT_NE(channel, nullptr);
    const auto expected_channel_id = channel->channel_id();

    const uint32_t oversized_frame_len =
        endian::to_little(static_cast<uint32_t>(kMaxBundleSize + 1));
    auto* len_bytes = reinterpret_cast<const std::byte*>(&oversized_frame_len);
    auto send_result =
        client_sock->send(std::span<const std::byte>(len_bytes, sizeof(oversized_frame_len)));
    ASSERT_TRUE(send_result.has_value()) << send_result.error().message();
    ASSERT_EQ(*send_result, sizeof(oversized_frame_len));

    ASSERT_TRUE(poll_until(
        dispatcher_, [&] { return callback_seen.load(std::memory_order_acquire); },
        std::chrono::milliseconds(1000)));

    EXPECT_EQ(callback_channel_id.load(std::memory_order_relaxed), expected_channel_id);
    EXPECT_EQ(callback_state.load(std::memory_order_relaxed),
              static_cast<int>(ChannelState::Condemned));
    EXPECT_TRUE(callback_removed_from_active_set.load(std::memory_order_relaxed));
    EXPECT_EQ(ni_.channel_count(), 0u);
}

TEST_F(NetworkInterfaceTest, PrepareForShutdownDoesNotInvokeDisconnectCallback)
{
    ASSERT_TRUE(ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    std::atomic<uint32_t> disconnect_callbacks{0};
    ni_.set_disconnect_callback([&](Channel&)
                                { disconnect_callbacks.fetch_add(1, std::memory_order_relaxed); });

    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    auto connect_result = client_sock->connect(ni_.tcp_address());
    ASSERT_TRUE(connect_result.has_value() ||
                connect_result.error().code() == ErrorCode::WouldBlock);

    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

    ni_.prepare_for_shutdown();
    poll_until(dispatcher_, [&] { return false; }, std::chrono::milliseconds(20));

    EXPECT_EQ(disconnect_callbacks.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(ni_.channel_count(), 0u);
}

TEST_F(NetworkInterfaceTest, CondemnedTcpChannelReclaimsExpandedRecvBufferImmediately)
{
    ASSERT_TRUE(ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    auto connect_result = client_sock->connect(ni_.tcp_address());
    ASSERT_TRUE(connect_result.has_value() ||
                connect_result.error().code() == ErrorCode::WouldBlock);

    ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));

    auto client_addr = client_sock->local_address();
    ASSERT_TRUE(client_addr.has_value()) << client_addr.error().message();

    auto* channel = dynamic_cast<TcpChannel*>(ni_.find_channel(*client_addr));
    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(channel->recv_buffer_capacity(), TcpChannel::kInitialRecvBufferSize);

    constexpr std::size_t kFrameLength = 48 * 1024;
    constexpr std::size_t kPartialPayload = 24 * 1024;
    uint32_t frame_len_le = endian::to_little(static_cast<uint32_t>(kFrameLength));

    std::vector<std::byte> partial_frame;
    partial_frame.reserve(sizeof(frame_len_le) + kPartialPayload);
    auto* len_bytes = reinterpret_cast<const std::byte*>(&frame_len_le);
    partial_frame.insert(partial_frame.end(), len_bytes, len_bytes + sizeof(frame_len_le));
    partial_frame.insert(partial_frame.end(), kPartialPayload, std::byte{'x'});

    std::size_t sent_total = 0;
    while (sent_total < partial_frame.size())
    {
        auto sent = client_sock->send(std::span<const std::byte>(
            partial_frame.data() + sent_total, partial_frame.size() - sent_total));
        ASSERT_TRUE(sent.has_value()) << sent.error().message();
        ASSERT_GT(*sent, 0u);
        sent_total += *sent;
    }

    ASSERT_TRUE(poll_until(dispatcher_,
                           [&]
                           {
                               return channel->recv_buffer_capacity() >
                                          TcpChannel::kInitialRecvBufferSize &&
                                      channel->recv_buffer_size() > 0;
                           }));

    ni_.prepare_for_shutdown();

    EXPECT_EQ(ni_.channel_count(), 0u);
    EXPECT_EQ(channel->recv_buffer_capacity(), TcpChannel::kInitialRecvBufferSize);
    EXPECT_EQ(channel->recv_buffer_size(), 0u);
    EXPECT_EQ(channel->write_buffer_capacity(), TcpChannel::kInitialWriteBufferSize);
    EXPECT_EQ(channel->write_buffer_size(), 0u);
}

// ============================================================================
// RUDP tests
// ============================================================================

TEST_F(NetworkInterfaceTest, StartRudpServer)
{
    auto result = ni_.start_rudp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_NE(ni_.rudp_address().port(), 0);
}

TEST_F(NetworkInterfaceTest, ConnectRudpCreatesChannel)
{
    // Server NI
    EventDispatcher server_disp{"rudp_server"};
    server_disp.set_max_poll_wait(Milliseconds(1));
    NetworkInterface server_ni(server_disp);

    auto sr = server_ni.start_rudp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(sr.has_value()) << sr.error().message();

    // Client NI connects to server
    auto cr = ni_.connect_rudp(server_ni.rudp_address());
    ASSERT_TRUE(cr.has_value()) << cr.error().message();
    EXPECT_NE(*cr, nullptr);
    EXPECT_EQ(ni_.channel_count(), 1u);
}

TEST_F(NetworkInterfaceTest, RudpServerAutoCreatesChannelOnFirstDatagram)
{
    auto sr = ni_.start_rudp_server(Address("127.0.0.1", 0));
    ASSERT_TRUE(sr.has_value());
    auto server_addr = ni_.rudp_address();

    // Send a raw datagram to the server from a plain UDP socket
    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());
    Address any(0, 0);
    (void)sender->bind(any);

    // Minimal RUDP packet: flags=0x02 (kFlagHasSeq), seq=1, no ack bits
    std::array<std::byte, 9> pkt{};
    pkt[0] = std::byte{rudp::kFlagHasSeq};  // flags
    pkt[1] = std::byte{0};
    pkt[2] = std::byte{0};  // seq hi
    pkt[3] = std::byte{0};
    pkt[4] = std::byte{1};  // seq lo (=1)
    pkt[5] = std::byte{0};
    pkt[6] = std::byte{0};  // ack hi
    pkt[7] = std::byte{0};
    pkt[8] = std::byte{0};  // ack lo

    (void)sender->send_to(pkt, server_addr);

    // Dispatcher should accept the datagram and create a channel
    EXPECT_TRUE(poll_until(dispatcher_, [&] { return ni_.channel_count() >= 1u; }));
}

TEST_F(NetworkInterfaceTest, ConnectRudpIsIdempotent)
{
    EventDispatcher server_disp{"rudp_idem"};
    server_disp.set_max_poll_wait(Milliseconds(1));
    NetworkInterface server_ni(server_disp);
    ASSERT_TRUE(server_ni.start_rudp_server(Address("127.0.0.1", 0)).has_value());

    auto cr1 = ni_.connect_rudp(server_ni.rudp_address());
    auto cr2 = ni_.connect_rudp(server_ni.rudp_address());
    ASSERT_TRUE(cr1.has_value());
    ASSERT_TRUE(cr2.has_value());
    EXPECT_EQ(*cr1, *cr2);  // same channel reused
    EXPECT_EQ(ni_.channel_count(), 1u);
}

TEST_F(NetworkInterfaceTest, RudpRateLimitedDatagramsAlsoConsumePerPollBudget)
{
    ASSERT_TRUE(ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());
    ni_.set_rate_limit(1);

    constexpr uint32_t kDatagramCount = 1500;  // intentionally above the current RUDP budget
    std::vector<Socket> senders;
    senders.reserve(kDatagramCount);

    std::array<std::byte, 9> pkt{};
    pkt[0] = std::byte{rudp::kFlagHasSeq};
    pkt[4] = std::byte{1};

    for (uint32_t i = 0; i < kDatagramCount; ++i)
    {
        auto sender = Socket::create_udp();
        ASSERT_TRUE(sender.has_value()) << sender.error().message();
        ASSERT_TRUE(sender->bind(Address("127.0.0.1", 0)).has_value());

        auto sent = sender->send_to(pkt, ni_.rudp_address());
        ASSERT_TRUE(sent.has_value()) << sent.error().message();
        ASSERT_EQ(*sent, pkt.size());

        senders.push_back(std::move(*sender));
    }

    dispatcher_.process_once();
    EXPECT_EQ(ni_.channel_count(), 1u);

    ni_.set_rate_limit(0);
    ASSERT_TRUE(poll_until(
        dispatcher_, [&] { return ni_.channel_count() > 1u; }, std::chrono::milliseconds(500)));
}

TEST_F(NetworkInterfaceTest, RudpServerDefaultProfileKeepsCongestionControl)
{
    EventDispatcher server_disp{"rudp_profile_default_server"};
    server_disp.set_max_poll_wait(Milliseconds(1));
    NetworkInterface server_ni(server_disp);
    Channel* accepted = nullptr;
    server_ni.set_accept_callback([&accepted](Channel& ch) { accepted = &ch; });
    ASSERT_TRUE(server_ni.start_rudp_server(Address("127.0.0.1", 0)).has_value());

    auto conn = ni_.connect_rudp(server_ni.rudp_address());
    ASSERT_TRUE(conn.has_value());
    ASSERT_TRUE((*conn)->send_message(NetTestMsg{1}).has_value());

    ASSERT_TRUE(poll_until(server_disp, [&] { return accepted != nullptr; }));

    auto* server_ch = static_cast<ReliableUdpChannel*>(accepted);
    EXPECT_FALSE(server_ch->nocwnd());
}

TEST_F(NetworkInterfaceTest, RudpServerClusterProfileDisablesCongestionControl)
{
    EventDispatcher server_disp{"rudp_profile_cluster_server"};
    server_disp.set_max_poll_wait(Milliseconds(1));
    NetworkInterface server_ni(server_disp);
    Channel* accepted = nullptr;
    server_ni.set_accept_callback([&accepted](Channel& ch) { accepted = &ch; });
    ASSERT_TRUE(
        server_ni
            .start_rudp_server(Address("127.0.0.1", 0), NetworkInterface::cluster_rudp_profile())
            .has_value());

    auto conn = ni_.connect_rudp(server_ni.rudp_address());
    ASSERT_TRUE(conn.has_value());
    ASSERT_TRUE((*conn)->send_message(NetTestMsg{1}).has_value());

    ASSERT_TRUE(poll_until(server_disp, [&] { return accepted != nullptr; }));

    auto* server_ch = static_cast<ReliableUdpChannel*>(accepted);
    EXPECT_TRUE(server_ch->nocwnd());
    EXPECT_GT(server_ch->effective_window(), 1u);
}
