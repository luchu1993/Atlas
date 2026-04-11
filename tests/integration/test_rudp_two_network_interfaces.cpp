// Integration test: Bidirectional RUDP messaging between two NetworkInterface
// instances, exercising the full NetworkInterface → ReliableUdpChannel path.
//
// Scenarios:
//   1. Client connects to server via RUDP; sends a message; server receives it.
//   2. Server sends a reply back; client receives it (full round-trip).
//   3. Multiple messages in one direction are received in order.
//   4. Large (fragmented) payload is reassembled correctly end-to-end.
//   5. connect_rudp_nocwnd creates a channel with congestion control disabled.

#include "network/event_dispatcher.hpp"
#include "network/message.hpp"
#include "network/network_interface.hpp"
#include "network/reliable_udp.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace atlas;

// ============================================================================
// Message definitions
// ============================================================================

struct RudpIntMsg
{
    uint32_t seq{0};
    std::string text;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{500, "RudpIntMsg", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(seq);
        w.write_string(text);
    }

    static auto deserialize(BinaryReader& r) -> Result<RudpIntMsg>
    {
        RudpIntMsg msg;
        auto s = r.read<uint32_t>();
        if (!s)
            return s.error();
        msg.seq = *s;
        auto t = r.read_string();
        if (!t)
            return t.error();
        msg.text = std::move(*t);
        return msg;
    }
};

struct RudpLargeMsg
{
    std::vector<uint8_t> payload;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{501, "RudpLargeMsg", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(static_cast<uint32_t>(payload.size()));
        for (auto b : payload)
            w.write<uint8_t>(b);
    }

    static auto deserialize(BinaryReader& r) -> Result<RudpLargeMsg>
    {
        auto sz = r.read<uint32_t>();
        if (!sz)
            return sz.error();
        RudpLargeMsg msg;
        msg.payload.resize(*sz);
        for (uint32_t i = 0; i < *sz; ++i)
        {
            auto b = r.read<uint8_t>();
            if (!b)
                return b.error();
            msg.payload[i] = *b;
        }
        return msg;
    }
};

// ============================================================================
// Helpers
// ============================================================================

template <typename Pred>
static bool poll_both_until(EventDispatcher& a, EventDispatcher& b, Pred pred,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        a.process_once();
        b.process_once();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ============================================================================
// Fixture
// ============================================================================

class RudpTwoNiTest : public ::testing::Test
{
protected:
    EventDispatcher server_disp_{"server"};
    EventDispatcher client_disp_{"client"};
    NetworkInterface server_ni_{server_disp_};
    NetworkInterface client_ni_{client_disp_};

    void SetUp() override
    {
        server_disp_.set_max_poll_wait(Milliseconds(1));
        client_disp_.set_max_poll_wait(Milliseconds(1));
    }
};

// ============================================================================
// Test 1: Client sends message → server receives it
// ============================================================================

TEST_F(RudpTwoNiTest, ClientToServerOneMessage)
{
    ASSERT_TRUE(server_ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());
    auto server_addr = server_ni_.rudp_address();

    std::atomic<bool> received{false};
    std::atomic<uint32_t> received_seq{0};

    server_ni_.interface_table().register_typed_handler<RudpIntMsg>(
        [&](const Address&, Channel*, const RudpIntMsg& msg)
        {
            received_seq.store(msg.seq, std::memory_order_relaxed);
            received.store(true, std::memory_order_release);
        });

    auto conn = client_ni_.connect_rudp(server_addr);
    ASSERT_TRUE(conn.has_value()) << conn.error().message();

    auto* ch = *conn;
    auto send_result = ch->send_message(RudpIntMsg{42, "hello"});
    ASSERT_TRUE(send_result.has_value()) << send_result.error().message();

    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return received.load(std::memory_order_acquire); }))
        << "Server did not receive the RUDP message";

    EXPECT_EQ(received_seq.load(), 42u);
}

// ============================================================================
// Test 2: Full round-trip (client → server → client)
// ============================================================================

TEST_F(RudpTwoNiTest, RoundTripRequestReply)
{
    ASSERT_TRUE(server_ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());
    auto server_addr = server_ni_.rudp_address();

    // Server echoes the message back using the incoming channel.
    server_ni_.interface_table().register_typed_handler<RudpIntMsg>(
        [&](const Address&, Channel* ch, const RudpIntMsg& msg)
        {
            if (ch)
                (void)ch->send_message(RudpIntMsg{msg.seq + 1000, "echo:" + msg.text});
        });

    std::atomic<bool> reply_received{false};
    std::atomic<uint32_t> reply_seq{0};

    client_ni_.interface_table().register_typed_handler<RudpIntMsg>(
        [&](const Address&, Channel*, const RudpIntMsg& msg)
        {
            reply_seq.store(msg.seq, std::memory_order_relaxed);
            reply_received.store(true, std::memory_order_release);
        });

    auto conn = client_ni_.connect_rudp(server_addr);
    ASSERT_TRUE(conn.has_value());

    // Wait for the server to create a reverse channel (first datagram triggers it).
    (void)(*conn)->send_message(RudpIntMsg{7, "ping"});

    ASSERT_TRUE(poll_both_until(
        server_disp_, client_disp_, [&] { return server_ni_.channel_count() >= 1u; },
        std::chrono::milliseconds(1000)));

    // Allow the round-trip to complete.
    ASSERT_TRUE(poll_both_until(
        server_disp_, client_disp_, [&] { return reply_received.load(std::memory_order_acquire); },
        std::chrono::milliseconds(2000)))
        << "Client did not receive the echo reply";

    EXPECT_EQ(reply_seq.load(), 1007u);  // 7 + 1000
}

// ============================================================================
// Test 3: Multiple messages received in order
// ============================================================================

TEST_F(RudpTwoNiTest, MultipleMessagesInOrder)
{
    ASSERT_TRUE(server_ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());

    std::vector<uint32_t> received_seqs;
    std::atomic<int> count{0};
    constexpr int kCount = 5;

    server_ni_.interface_table().register_typed_handler<RudpIntMsg>(
        [&](const Address&, Channel*, const RudpIntMsg& msg)
        {
            received_seqs.push_back(msg.seq);
            count.fetch_add(1, std::memory_order_release);
        });

    auto conn = client_ni_.connect_rudp(server_ni_.rudp_address());
    ASSERT_TRUE(conn.has_value());

    auto* ch = *conn;
    // Disable congestion window so all messages are sent without blocking.
    ch->set_nocwnd(true);

    for (uint32_t i = 0; i < kCount; ++i)
    {
        (void)ch->send_message(RudpIntMsg{i, "msg"});
    }

    ASSERT_TRUE(poll_both_until(
        server_disp_, client_disp_, [&] { return count.load(std::memory_order_acquire) >= kCount; },
        std::chrono::milliseconds(2000)));

    ASSERT_EQ(static_cast<int>(received_seqs.size()), kCount);
    for (int i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(received_seqs[i], static_cast<uint32_t>(i)) << "Order mismatch at index " << i;
    }
}

// ============================================================================
// Test 4: Large (fragmented) payload reassembled end-to-end
// ============================================================================

TEST_F(RudpTwoNiTest, LargePayloadFragmentedAndReassembled)
{
    ASSERT_TRUE(server_ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());

    constexpr uint32_t kPayloadSize = 8000;

    std::atomic<bool> received{false};
    std::vector<uint8_t> received_payload;

    server_ni_.interface_table().register_typed_handler<RudpLargeMsg>(
        [&](const Address&, Channel*, const RudpLargeMsg& msg)
        {
            received_payload = msg.payload;
            received.store(true, std::memory_order_release);
        });

    auto conn = client_ni_.connect_rudp(server_ni_.rudp_address());
    ASSERT_TRUE(conn.has_value());

    auto* ch = *conn;
    // Disable cwnd so all fragments are sent in one burst.
    ch->set_nocwnd(true);

    RudpLargeMsg big;
    big.payload.resize(kPayloadSize);
    for (uint32_t i = 0; i < kPayloadSize; ++i)
        big.payload[i] = static_cast<uint8_t>(i % 251);

    auto send_result = ch->send_message(big);
    ASSERT_TRUE(send_result.has_value()) << send_result.error().message();

    ASSERT_TRUE(poll_both_until(
        server_disp_, client_disp_, [&] { return received.load(std::memory_order_acquire); },
        std::chrono::milliseconds(3000)))
        << "Large payload was never reassembled on the server";

    ASSERT_EQ(received_payload.size(), kPayloadSize);
    for (uint32_t i = 0; i < kPayloadSize; ++i)
    {
        EXPECT_EQ(received_payload[i], static_cast<uint8_t>(i % 251))
            << "Payload mismatch at byte " << i;
    }
}

// ============================================================================
// Test 5: connect_rudp_nocwnd creates channel with nocwnd=true
// ============================================================================

TEST_F(RudpTwoNiTest, ConnectRudpNocwndCreatesNocwndChannel)
{
    ASSERT_TRUE(server_ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());

    auto conn = client_ni_.connect_rudp_nocwnd(server_ni_.rudp_address());
    ASSERT_TRUE(conn.has_value()) << conn.error().message();

    auto* rudp_ch = dynamic_cast<ReliableUdpChannel*>(*conn);
    ASSERT_NE(rudp_ch, nullptr);
    EXPECT_TRUE(rudp_ch->nocwnd());
    // With nocwnd the effective window equals the full send window.
    EXPECT_GT(rudp_ch->effective_window(), 1u);
}

// ============================================================================
// Test 6: Server channel count increases on first client datagram
// ============================================================================

TEST_F(RudpTwoNiTest, ServerChannelCountIncrementsOnConnect)
{
    ASSERT_TRUE(server_ni_.start_rudp_server(Address("127.0.0.1", 0)).has_value());
    EXPECT_EQ(server_ni_.channel_count(), 0u);

    server_ni_.interface_table().register_typed_handler<RudpIntMsg>(
        [](const Address&, Channel*, const RudpIntMsg&) {});

    auto conn = client_ni_.connect_rudp(server_ni_.rudp_address());
    ASSERT_TRUE(conn.has_value());
    (void)(*conn)->send_message(RudpIntMsg{0, "probe"});

    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return server_ni_.channel_count() >= 1u; }))
        << "Server channel count did not increase after first RUDP datagram";
}
