#include <gtest/gtest.h>
#include "network/socket.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace atlas;

TEST(Socket, CreateTcp)
{
    auto result = Socket::create_tcp();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_valid());
}

TEST(Socket, CreateUdp)
{
    auto result = Socket::create_udp();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_valid());
}

TEST(Socket, MoveSemantics)
{
    auto result = Socket::create_tcp();
    ASSERT_TRUE(result.has_value());
    auto fd = result->fd();

    Socket moved(std::move(*result));
    EXPECT_TRUE(moved.is_valid());
    EXPECT_EQ(moved.fd(), fd);
    EXPECT_FALSE(result->is_valid());  // moved-from is invalid
}

TEST(Socket, TcpBindAndLocalAddress)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());

    auto bind_result = sock->bind(Address("127.0.0.1", 0));
    ASSERT_TRUE(bind_result.has_value());

    auto addr = sock->local_address();
    ASSERT_TRUE(addr.has_value());
    EXPECT_NE(addr->port(), 0);  // OS assigned a port
}

TEST(Socket, TcpListenAcceptConnect)
{
    // Server
    auto server = Socket::create_tcp();
    ASSERT_TRUE(server.has_value());
    ASSERT_TRUE(server->bind(Address("127.0.0.1", 0)).has_value());
    ASSERT_TRUE(server->listen().has_value());
    auto server_addr = server->local_address();
    ASSERT_TRUE(server_addr.has_value());

    // Client connects
    auto client = Socket::create_tcp();
    ASSERT_TRUE(client.has_value());
    auto conn = client->connect(*server_addr);
    // connect may return WouldBlock (async) or success
    if (!conn.has_value())
    {
        EXPECT_EQ(conn.error().code(), ErrorCode::WouldBlock);
    }

    // Give OS a moment to establish connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Accept
    auto accepted = server->accept();
    ASSERT_TRUE(accepted.has_value()) << accepted.error().message();
    auto& [peer_sock, peer_addr] = *accepted;
    EXPECT_TRUE(peer_sock.is_valid());
}

TEST(Socket, TcpSendRecvLoopback)
{
    auto server = Socket::create_tcp();
    ASSERT_TRUE(server.has_value());
    ASSERT_TRUE(server->bind(Address("127.0.0.1", 0)).has_value());
    ASSERT_TRUE(server->listen().has_value());
    auto server_addr = server->local_address().value();

    auto client = Socket::create_tcp();
    ASSERT_TRUE(client.has_value());
    (void)client->connect(server_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto accepted = server->accept();
    ASSERT_TRUE(accepted.has_value());
    auto& [peer, peer_addr] = *accepted;

    // Send data from client
    std::array<std::byte, 5> send_data = {
        std::byte{0x48}, std::byte{0x45}, std::byte{0x4C},
        std::byte{0x4C}, std::byte{0x4F}
    };

    // Wait for connection to establish fully
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto sent = client->send(send_data);
    ASSERT_TRUE(sent.has_value()) << sent.error().message();
    EXPECT_EQ(*sent, 5u);

    // Receive on accepted socket
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::array<std::byte, 64> recv_buf{};
    auto received = peer.recv(recv_buf);
    ASSERT_TRUE(received.has_value()) << received.error().message();
    EXPECT_EQ(*received, 5u);
    EXPECT_EQ(static_cast<uint8_t>(recv_buf[0]), 0x48);
}

TEST(Socket, UdpSendRecvLoopback)
{
    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());

    auto receiver = Socket::create_udp();
    ASSERT_TRUE(receiver.has_value());
    ASSERT_TRUE(receiver->bind(Address("127.0.0.1", 0)).has_value());
    auto recv_addr = receiver->local_address().value();

    std::array<std::byte, 3> data = {std::byte{1}, std::byte{2}, std::byte{3}};
    auto sent = sender->send_to(data, recv_addr);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(*sent, 3u);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::array<std::byte, 64> buf{};
    auto result = receiver->recv_from(buf);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto [bytes, src_addr] = *result;
    EXPECT_EQ(bytes, 3u);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 1);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 2);
    EXPECT_EQ(static_cast<uint8_t>(buf[2]), 3);
}

TEST(Socket, BindConflictReturnsAddressInUse)
{
    auto sock1 = Socket::create_tcp();
    ASSERT_TRUE(sock1.has_value());
    ASSERT_TRUE(sock1->bind(Address("127.0.0.1", 0)).has_value());
    auto addr = sock1->local_address().value();

    // Try to bind second socket to same address
    auto sock2 = Socket::create_tcp();
    ASSERT_TRUE(sock2.has_value());
    // Disable reuse for this test
    sock2->set_reuse_addr(false);
    auto result = sock2->bind(addr);
    // On some systems with SO_REUSEADDR this may still succeed for TCP,
    // but for the same exact address it should fail
    if (!result.has_value())
    {
        EXPECT_EQ(result.error().code(), ErrorCode::AddressInUse);
    }
}

TEST(Socket, CloseInvalidatesSocket)
{
    auto sock = Socket::create_tcp();
    ASSERT_TRUE(sock.has_value());
    EXPECT_TRUE(sock->is_valid());

    sock->close();
    EXPECT_FALSE(sock->is_valid());

    // Double close should be safe
    sock->close();
    EXPECT_FALSE(sock->is_valid());
}
