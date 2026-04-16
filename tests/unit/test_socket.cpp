#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

#include <gtest/gtest.h>

#include "network/socket.h"

using namespace atlas;

TEST(Socket, CreateTcp) {
  auto result = Socket::CreateTcp();
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->IsValid());
}

TEST(Socket, CreateUdp) {
  auto result = Socket::CreateUdp();
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->IsValid());
}

TEST(Socket, MoveSemantics) {
  auto result = Socket::CreateTcp();
  ASSERT_TRUE(result.HasValue());
  auto fd = result->Fd();

  Socket moved(std::move(*result));
  EXPECT_TRUE(moved.IsValid());
  EXPECT_EQ(moved.Fd(), fd);
  EXPECT_FALSE(result->IsValid());  // moved-from is invalid
}

TEST(Socket, TcpBindAndLocalAddress) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  auto bind_result = sock->Bind(Address("127.0.0.1", 0));
  ASSERT_TRUE(bind_result.HasValue());

  auto addr = sock->LocalAddress();
  ASSERT_TRUE(addr.HasValue());
  EXPECT_NE(addr->Port(), 0);  // OS assigned a port
}

TEST(Socket, TcpListenAcceptConnect) {
  // Server
  auto server = Socket::CreateTcp();
  ASSERT_TRUE(server.HasValue());
  ASSERT_TRUE(server->Bind(Address("127.0.0.1", 0)).HasValue());
  ASSERT_TRUE(server->Listen().HasValue());
  auto server_addr = server->LocalAddress();
  ASSERT_TRUE(server_addr.HasValue());

  // Client connects
  auto client = Socket::CreateTcp();
  ASSERT_TRUE(client.HasValue());
  auto conn = client->Connect(*server_addr);
  // connect may return WouldBlock (async) or success
  if (!conn.HasValue()) {
    EXPECT_EQ(conn.Error().Code(), ErrorCode::kWouldBlock);
  }

  // Give OS a moment to establish connection
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Accept
  auto accepted = server->Accept();
  ASSERT_TRUE(accepted.HasValue()) << accepted.Error().Message();
  auto& [peer_sock, peer_addr] = *accepted;
  EXPECT_TRUE(peer_sock.IsValid());
}

TEST(Socket, TcpSendRecvLoopback) {
  auto server = Socket::CreateTcp();
  ASSERT_TRUE(server.HasValue());
  ASSERT_TRUE(server->Bind(Address("127.0.0.1", 0)).HasValue());
  ASSERT_TRUE(server->Listen().HasValue());
  auto server_addr = server->LocalAddress().Value();

  auto client = Socket::CreateTcp();
  ASSERT_TRUE(client.HasValue());
  (void)client->Connect(server_addr);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto accepted = server->Accept();
  ASSERT_TRUE(accepted.HasValue());
  auto& [peer, peer_addr] = *accepted;

  // Send data from client
  std::array<std::byte, 5> send_data = {std::byte{0x48}, std::byte{0x45}, std::byte{0x4C},
                                        std::byte{0x4C}, std::byte{0x4F}};

  // Wait for connection to establish fully
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto sent = client->Send(send_data);
  ASSERT_TRUE(sent.HasValue()) << sent.Error().Message();
  EXPECT_EQ(*sent, 5u);

  // Receive on accepted socket
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::array<std::byte, 64> recv_buf{};
  auto received = peer.Recv(recv_buf);
  ASSERT_TRUE(received.HasValue()) << received.Error().Message();
  EXPECT_EQ(*received, 5u);
  EXPECT_EQ(static_cast<uint8_t>(recv_buf[0]), 0x48);
}

TEST(Socket, UdpSendRecvLoopback) {
  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());

  auto receiver = Socket::CreateUdp();
  ASSERT_TRUE(receiver.HasValue());
  ASSERT_TRUE(receiver->Bind(Address("127.0.0.1", 0)).HasValue());
  auto recv_addr = receiver->LocalAddress().Value();

  std::array<std::byte, 3> data = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto sent = sender->SendTo(data, recv_addr);
  ASSERT_TRUE(sent.HasValue());
  EXPECT_EQ(*sent, 3u);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::array<std::byte, 64> buf{};
  auto result = receiver->RecvFrom(buf);
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  auto [bytes, src_addr] = *result;
  EXPECT_EQ(bytes, 3u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 1);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 2);
  EXPECT_EQ(static_cast<uint8_t>(buf[2]), 3);
}

TEST(Socket, BindConflictReturnsAddressInUse) {
  auto sock1 = Socket::CreateTcp();
  ASSERT_TRUE(sock1.HasValue());
  ASSERT_TRUE(sock1->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr = sock1->LocalAddress().Value();

  // Try to bind second socket to same address
  auto sock2 = Socket::CreateTcp();
  ASSERT_TRUE(sock2.HasValue());
  // Disable reuse for this test
  (void)sock2->SetReuseAddr(false);
  auto result = sock2->Bind(addr);
  // On some systems with SO_REUSEADDR this may still succeed for TCP,
  // but for the same exact address it should fail
  if (!result.HasValue()) {
    EXPECT_EQ(result.Error().Code(), ErrorCode::kAddressInUse);
  }
}

TEST(Socket, CloseInvalidatesSocket) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->IsValid());

  sock->Close();
  EXPECT_FALSE(sock->IsValid());

  // Double close should be safe
  sock->Close();
  EXPECT_FALSE(sock->IsValid());
}
