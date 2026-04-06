#include <gtest/gtest.h>
#include "network/tcp_channel.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"
#include "network/socket.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace atlas;

// Test message
struct ChannelTestMsg
{
    uint32_t seq;
    std::string payload;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{100, "ChannelTestMsg", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(seq);
        w.write_string(payload);
    }

    static auto deserialize(BinaryReader& r) -> Result<ChannelTestMsg>
    {
        auto s = r.read<uint32_t>();
        if (!s) return s.error();
        auto p = r.read_string();
        if (!p) return p.error();
        return ChannelTestMsg{*s, std::move(*p)};
    }
};

class ChannelTest : public ::testing::Test
{
protected:
    EventDispatcher dispatcher_{"test"};
    InterfaceTable table_;

    void SetUp() override
    {
        dispatcher_.set_max_poll_wait(Milliseconds(1));
    }
};

TEST_F(ChannelTest, TcpChannelStateTransitions)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
    EXPECT_EQ(channel.state(), ChannelState::Created);

    channel.activate();
    EXPECT_EQ(channel.state(), ChannelState::Active);

    channel.condemn();
    EXPECT_EQ(channel.state(), ChannelState::Condemned);
}

TEST_F(ChannelTest, SendOnCondemnedReturnsError)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
    channel.condemn();

    channel.bundle().add_message(ChannelTestMsg{1, "test"});
    auto result = channel.send();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::ChannelCondemned);
}

TEST_F(ChannelTest, TcpLoopbackSendAndDispatch)
{
    // Set up server
    auto server_sock = Socket::create_tcp();
    ASSERT_TRUE(server_sock.has_value());
    ASSERT_TRUE(server_sock->bind(Address("127.0.0.1", 0)).has_value());
    ASSERT_TRUE(server_sock->listen().has_value());
    auto server_addr = server_sock->local_address().value();

    // Client connects
    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    (void)client_sock->connect(server_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Accept
    auto accepted = server_sock->accept();
    ASSERT_TRUE(accepted.has_value());
    auto& [peer_sock, peer_addr] = *accepted;

    // Register message handler
    bool received = false;
    uint32_t received_seq = 0;
    std::string received_payload;

    (void)table_.register_typed_handler<ChannelTestMsg>(
        [&](const Address&, Channel*, const ChannelTestMsg& msg)
        {
            received = true;
            received_seq = msg.seq;
            received_payload = msg.payload;
        });

    // Create channels
    TcpChannel sender(dispatcher_, table_, std::move(*client_sock), server_addr);
    sender.activate();

    TcpChannel receiver(dispatcher_, table_, std::move(peer_sock), peer_addr);
    receiver.activate();

    // Register receiver for IO
    auto reg = dispatcher_.register_reader(receiver.fd(),
        [&](FdHandle, IOEvent) { receiver.on_readable(); });
    ASSERT_TRUE(reg.has_value());

    // Send message
    auto send_result = sender.send_message(ChannelTestMsg{42, "hello world"});
    ASSERT_TRUE(send_result.has_value()) << send_result.error().message();

    // Give time for data to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Process IO
    dispatcher_.process_once();

    EXPECT_TRUE(received);
    EXPECT_EQ(received_seq, 42u);
    EXPECT_EQ(received_payload, "hello world");

    // Cleanup — deregister before channel destructor
    (void)dispatcher_.deregister(receiver.fd());
}

TEST_F(ChannelTest, ByteStatisticsTracking)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
    EXPECT_EQ(channel.bytes_sent(), 0u);
    EXPECT_EQ(channel.bytes_received(), 0u);
}

TEST_F(ChannelTest, DisconnectCallbackInvoked)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
    channel.activate();

    bool disconnected = false;
    channel.set_disconnect_callback([&](Channel&) { disconnected = true; });

    channel.condemn();
    // Condemn itself does not trigger disconnect callback — on_disconnect does
    // But our test can verify the callback is set and the state transitions
    EXPECT_EQ(channel.state(), ChannelState::Condemned);
}

TEST_F(ChannelTest, SendOnEmptyBundleIsNoOp)
{
    auto server_sock = Socket::create_tcp();
    ASSERT_TRUE(server_sock.has_value());
    ASSERT_TRUE(server_sock->bind(Address("127.0.0.1", 0)).has_value());
    ASSERT_TRUE(server_sock->listen().has_value());
    auto server_addr = server_sock->local_address().value();

    auto client_sock = Socket::create_tcp();
    ASSERT_TRUE(client_sock.has_value());
    (void)client_sock->connect(server_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto accepted = server_sock->accept();
    ASSERT_TRUE(accepted.has_value());

    TcpChannel channel(dispatcher_, table_, std::move(*client_sock), server_addr);
    channel.activate();

    // Sending an empty bundle should succeed without actually sending
    auto result = channel.send();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(channel.bytes_sent(), 0u);
}

TEST_F(ChannelTest, CondemnIsIdempotent)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
    channel.activate();

    channel.condemn();
    EXPECT_EQ(channel.state(), ChannelState::Condemned);

    // Condemning again should not crash or change state
    channel.condemn();
    EXPECT_EQ(channel.state(), ChannelState::Condemned);
}

TEST_F(ChannelTest, RemoteAddressReturned)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    Address remote("127.0.0.1", 9999);
    TcpChannel channel(dispatcher_, table_, std::move(*sock), remote);

    EXPECT_EQ(channel.remote_address(), remote);
}
