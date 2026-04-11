// Integration test: TCP bidirectional message round-trip between two
// NetworkInterface instances running in the same event-loop thread.
//
// Scenario:
//   - "Server" NI binds a TCP listener.
//   - "Client" NI connects to it.
//   - Client sends EchoRequest; server echoes back EchoReply.
//   - Test verifies payload fidelity and bytes_sent / bytes_received statistics.

#include "network/channel.hpp"
#include "network/event_dispatcher.hpp"
#include "network/message.hpp"
#include "network/network_interface.hpp"
#include "network/tcp_channel.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace atlas;

// ============================================================================
// Message definitions
// ============================================================================

struct EchoRequest
{
    uint32_t id{0};
    std::string text;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{400, "EchoRequest", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(id);
        w.write_string(text);
    }

    static auto deserialize(BinaryReader& r) -> Result<EchoRequest>
    {
        EchoRequest msg;
        auto i = r.read<uint32_t>();
        if (!i)
            return i.error();
        msg.id = *i;
        auto t = r.read_string();
        if (!t)
            return t.error();
        msg.text = std::move(*t);
        return msg;
    }
};

struct EchoReply
{
    uint32_t id{0};
    std::string text;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{401, "EchoReply", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(id);
        w.write_string(text);
    }

    static auto deserialize(BinaryReader& r) -> Result<EchoReply>
    {
        EchoReply msg;
        auto i = r.read<uint32_t>();
        if (!i)
            return i.error();
        msg.id = *i;
        auto t = r.read_string();
        if (!t)
            return t.error();
        msg.text = std::move(*t);
        return msg;
    }
};

// ============================================================================
// Helpers
// ============================================================================

// Drive both dispatchers until predicate returns true or deadline passes.
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

class TcpEchoRoundtripTest : public ::testing::Test
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
// Test: single request → reply
// ============================================================================

TEST_F(TcpEchoRoundtripTest, SingleRequestReply)
{
    // ---- Server setup -------------------------------------------------------
    ASSERT_TRUE(server_ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());
    auto server_addr = server_ni_.tcp_address();

    // Server echoes every EchoRequest back as an EchoReply on the same channel.
    server_ni_.interface_table().register_typed_handler<EchoRequest>(
        [&](const Address&, Channel* ch, const EchoRequest& req)
        {
            EchoReply reply{req.id, req.text};
            (void)ch->send_message(reply);
        });

    // ---- Client setup -------------------------------------------------------
    std::atomic<bool> reply_received{false};
    std::atomic<uint32_t> reply_id{0};
    std::string reply_text;

    client_ni_.interface_table().register_typed_handler<EchoReply>(
        [&](const Address&, Channel*, const EchoReply& rep)
        {
            reply_id.store(rep.id, std::memory_order_relaxed);
            reply_text = rep.text;
            reply_received.store(true, std::memory_order_release);
        });

    auto conn = client_ni_.connect_tcp(server_addr);
    ASSERT_TRUE(conn.has_value()) << conn.error().message();

    // Wait until the server has accepted the connection.
    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return server_ni_.channel_count() >= 1u; }));

    // Client sends the request.
    auto* channel = *conn;
    auto send_result = channel->send_message(EchoRequest{7, "hello"});
    ASSERT_TRUE(send_result.has_value()) << send_result.error().message();

    // Wait for the round-trip to complete.
    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return reply_received.load(std::memory_order_acquire); }))
        << "EchoReply was never received within the timeout";

    EXPECT_EQ(reply_id.load(), 7u);
    EXPECT_EQ(reply_text, "hello");
}

// ============================================================================
// Test: multiple sequential requests preserve order and identity
// ============================================================================

TEST_F(TcpEchoRoundtripTest, MultipleRequestsInOrder)
{
    ASSERT_TRUE(server_ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    server_ni_.interface_table().register_typed_handler<EchoRequest>(
        [&](const Address&, Channel* ch, const EchoRequest& req)
        { (void)ch->send_message(EchoReply{req.id, req.text}); });

    std::vector<uint32_t> received_ids;
    std::atomic<int> reply_count{0};
    constexpr int kRequests = 5;

    client_ni_.interface_table().register_typed_handler<EchoReply>(
        [&](const Address&, Channel*, const EchoReply& rep)
        {
            received_ids.push_back(rep.id);
            reply_count.fetch_add(1, std::memory_order_release);
        });

    auto conn = client_ni_.connect_tcp(server_ni_.tcp_address());
    ASSERT_TRUE(conn.has_value());
    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return server_ni_.channel_count() >= 1u; }));

    auto* ch = *conn;
    for (uint32_t i = 0; i < kRequests; ++i)
    {
        (void)ch->send_message(EchoRequest{i, std::to_string(i)});
    }

    ASSERT_TRUE(poll_both_until(
        server_disp_, client_disp_,
        [&] { return reply_count.load(std::memory_order_acquire) >= kRequests; },
        std::chrono::milliseconds(2000)));

    ASSERT_EQ(static_cast<int>(received_ids.size()), kRequests);
    for (int i = 0; i < kRequests; ++i)
    {
        EXPECT_EQ(received_ids[i], static_cast<uint32_t>(i));
    }
}

// ============================================================================
// Test: bytes_sent increases after sending
// ============================================================================

TEST_F(TcpEchoRoundtripTest, BytesSentTracked)
{
    ASSERT_TRUE(server_ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    // Server doesn't need to reply; we only care about the sender's stats.
    server_ni_.interface_table().register_typed_handler<EchoRequest>(
        [](const Address&, Channel*, const EchoRequest&) {});

    auto conn = client_ni_.connect_tcp(server_ni_.tcp_address());
    ASSERT_TRUE(conn.has_value());
    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return server_ni_.channel_count() >= 1u; }));

    auto* ch = *conn;
    EXPECT_EQ(ch->bytes_sent(), 0u);

    (void)ch->send_message(EchoRequest{1, "payload"});
    EXPECT_GT(ch->bytes_sent(), 0u);
}

// ============================================================================
// Test: prepare_for_shutdown condemns all server-side channels immediately,
// reducing channel_count() to zero.
// ============================================================================

TEST_F(TcpEchoRoundtripTest, ServerShutdownCondemnedServerChannels)
{
    ASSERT_TRUE(server_ni_.start_tcp_server(Address("127.0.0.1", 0)).has_value());

    auto conn = client_ni_.connect_tcp(server_ni_.tcp_address());
    ASSERT_TRUE(conn.has_value());

    // Wait for the server to accept the connection.
    ASSERT_TRUE(poll_both_until(server_disp_, client_disp_,
                                [&] { return server_ni_.channel_count() >= 1u; }));
    EXPECT_EQ(server_ni_.channel_count(), 1u);

    // Shut down the server: all accepted channels are moved to the condemned
    // list and removed from the active map immediately.
    server_ni_.prepare_for_shutdown();

    // channel_count() reflects only the live (active) map — it should be zero
    // immediately after prepare_for_shutdown().
    EXPECT_EQ(server_ni_.channel_count(), 0u);
}
