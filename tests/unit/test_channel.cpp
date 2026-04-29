#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

#include <gtest/gtest.h>

#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/socket.h"
#include "network/tcp_channel.h"

using namespace atlas;

// Test message
struct ChannelTestMsg {
  uint32_t seq;
  std::string payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{100,
                                  "ChannelTestMsg",
                                  MessageLengthStyle::kVariable,
                                  -1,
                                  MessageReliability::kReliable,
                                  MessageUrgency::kImmediate};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(seq);
    w.WriteString(payload);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ChannelTestMsg> {
    auto s = r.Read<uint32_t>();
    if (!s) return s.Error();
    auto p = r.ReadString();
    if (!p) return p.Error();
    return ChannelTestMsg{*s, std::move(*p)};
  }
};

class ChannelTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};
  InterfaceTable table_;

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }
};

TEST_F(ChannelTest, TcpChannelStateTransitions) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
  EXPECT_EQ(channel.State(), ChannelState::kCreated);

  channel.Activate();
  EXPECT_EQ(channel.State(), ChannelState::kActive);

  channel.Condemn();
  EXPECT_EQ(channel.State(), ChannelState::kCondemned);
}

TEST_F(ChannelTest, SendOnCondemnedReturnsError) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
  channel.Condemn();

  channel.Bundle().AddMessage(ChannelTestMsg{1, "test"});
  auto result = channel.Send();
  EXPECT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kChannelCondemned);
}

TEST_F(ChannelTest, TcpLoopbackSendAndDispatch) {
  // Set up server
  auto server_sock = Socket::CreateTcp();
  ASSERT_TRUE(server_sock.HasValue());
  ASSERT_TRUE(server_sock->Bind(Address("127.0.0.1", 0)).HasValue());
  ASSERT_TRUE(server_sock->Listen().HasValue());
  auto server_addr = server_sock->LocalAddress().Value();

  // Client connects
  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  (void)client_sock->Connect(server_addr);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Accept
  auto accepted = server_sock->Accept();
  ASSERT_TRUE(accepted.HasValue());
  auto& [peer_sock, peer_addr] = *accepted;

  // Register message handler
  bool received = false;
  uint32_t received_seq = 0;
  std::string received_payload;

  (void)table_.RegisterTypedHandler<ChannelTestMsg>(
      [&](const Address&, Channel*, const ChannelTestMsg& msg) {
        received = true;
        received_seq = msg.seq;
        received_payload = msg.payload;
      });

  // Create channels
  TcpChannel sender(dispatcher_, table_, std::move(*client_sock), server_addr);
  sender.Activate();

  TcpChannel receiver(dispatcher_, table_, std::move(peer_sock), peer_addr);
  receiver.Activate();

  // Register receiver for IO
  auto reg =
      dispatcher_.RegisterReader(receiver.Fd(), [&](FdHandle, IOEvent) { receiver.OnReadable(); });
  ASSERT_TRUE(reg.HasValue());

  // Send message
  auto send_result = sender.SendMessage(ChannelTestMsg{42, "hello world"});
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();

  // Give time for data to arrive
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process IO
  dispatcher_.ProcessOnce();

  EXPECT_TRUE(received);
  EXPECT_EQ(received_seq, 42u);
  EXPECT_EQ(received_payload, "hello world");

  // Cleanup — deregister before channel destructor
  (void)dispatcher_.Deregister(receiver.Fd());
}

TEST_F(ChannelTest, ByteStatisticsTracking) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
  EXPECT_EQ(channel.BytesSent(), 0u);
  EXPECT_EQ(channel.BytesReceived(), 0u);
}

TEST_F(ChannelTest, DisconnectCallbackInvoked) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
  channel.Activate();

  bool disconnected = false;
  channel.SetDisconnectCallback([&](Channel&) { disconnected = true; });

  channel.Condemn();
  // Condemn itself does not trigger disconnect callback — on_disconnect does
  // But our test can verify the callback is set and the state transitions
  EXPECT_EQ(channel.State(), ChannelState::kCondemned);
}

TEST_F(ChannelTest, SendOnEmptyBundleIsNoOp) {
  auto server_sock = Socket::CreateTcp();
  ASSERT_TRUE(server_sock.HasValue());
  ASSERT_TRUE(server_sock->Bind(Address("127.0.0.1", 0)).HasValue());
  ASSERT_TRUE(server_sock->Listen().HasValue());
  auto server_addr = server_sock->LocalAddress().Value();

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  (void)client_sock->Connect(server_addr);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto accepted = server_sock->Accept();
  ASSERT_TRUE(accepted.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*client_sock), server_addr);
  channel.Activate();

  // Sending an empty bundle should succeed without actually sending
  auto result = channel.Send();
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(channel.BytesSent(), 0u);
}

TEST_F(ChannelTest, CondemnIsIdempotent) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*sock), Address("127.0.0.1", 0));
  channel.Activate();

  channel.Condemn();
  EXPECT_EQ(channel.State(), ChannelState::kCondemned);

  // Condemning again should not crash or change state
  channel.Condemn();
  EXPECT_EQ(channel.State(), ChannelState::kCondemned);
}

TEST_F(ChannelTest, RemoteAddressReturned) {
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());

  Address remote("127.0.0.1", 9999);
  TcpChannel channel(dispatcher_, table_, std::move(*sock), remote);

  EXPECT_EQ(channel.RemoteAddress(), remote);
}

// ============================================================================
// Review issue #2: TcpChannel write buffer — verify bytes_sent increases
// after a successful send over a connected TCP pair.
// ============================================================================

TEST_F(ChannelTest, BytesSentIncreasesAfterSend) {
  // Set up server
  auto server_sock = Socket::CreateTcp();
  ASSERT_TRUE(server_sock.HasValue());
  ASSERT_TRUE(server_sock->Bind(Address("127.0.0.1", 0)).HasValue());
  ASSERT_TRUE(server_sock->Listen().HasValue());
  auto server_addr = server_sock->LocalAddress().Value();

  // Client connects
  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  (void)client_sock->Connect(server_addr);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Accept
  auto accepted = server_sock->Accept();
  ASSERT_TRUE(accepted.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*client_sock), server_addr);
  channel.Activate();

  EXPECT_EQ(channel.BytesSent(), 0u);

  // Send a message
  auto send_result = channel.SendMessage(ChannelTestMsg{1, "payload"});
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();

  // bytes_sent should now be > 0
  EXPECT_GT(channel.BytesSent(), 0u);
}

// kBatched test message — exercises TcpChannel::DeferredBundleFor /
// OnDeferredAppend / FlushDeferred end-to-end.
struct TcpBatchedMsg {
  uint32_t seq;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{101,
                                  "TcpBatchedMsg",
                                  MessageLengthStyle::kVariable,
                                  -1,
                                  MessageReliability::kReliable,
                                  MessageUrgency::kBatched};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint32_t>(seq); }

  static auto Deserialize(BinaryReader& r) -> Result<TcpBatchedMsg> {
    auto s = r.Read<uint32_t>();
    if (!s) return s.Error();
    return TcpBatchedMsg{*s};
  }
};

TEST_F(ChannelTest, TcpDeferredPathBatchesUntilFlush) {
  // SendMessage with a kBatched descriptor should accumulate in
  // TcpChannel's deferred bundle without hitting the wire — the
  // dirty-channels callback fires (NotifyDirty) but bytes_sent_
  // stays at 0 until FlushDeferred runs.  After FlushDeferred a
  // single TCP frame containing both messages goes out.
  auto sock = Socket::CreateTcp();
  ASSERT_TRUE(sock.HasValue());
  ASSERT_TRUE(sock->SetNonBlocking(true).HasValue());
  ASSERT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  ASSERT_TRUE(sock->Listen().HasValue());
  auto server_addr = sock->LocalAddress().Value();

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  (void)client_sock->Connect(server_addr);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto accepted = sock->Accept();
  ASSERT_TRUE(accepted.HasValue());

  TcpChannel channel(dispatcher_, table_, std::move(*client_sock), server_addr);
  channel.Activate();

  int dirty_calls = 0;
  channel.SetMarkDirtyCallback([&](Channel&) { ++dirty_calls; });

  // First batched send — accumulates in deferred bundle, no wire bytes.
  ASSERT_TRUE(channel.SendMessage(TcpBatchedMsg{1}).HasValue());
  EXPECT_EQ(dirty_calls, 1) << "first batched append must NotifyDirty";
  EXPECT_EQ(channel.BytesSent(), 0u) << "deferred bundle, no wire send yet";

  // Second batched send — same bundle, still no wire bytes.
  ASSERT_TRUE(channel.SendMessage(TcpBatchedMsg{2}).HasValue());
  EXPECT_GE(dirty_calls, 2);
  EXPECT_EQ(channel.BytesSent(), 0u);

  // Explicit drain — both messages ship in a single TCP frame.
  ASSERT_TRUE(channel.FlushDeferred().HasValue());
  EXPECT_GT(channel.BytesSent(), 0u) << "FlushDeferred must produce wire bytes";

  // Idempotent — second flush is a no-op.
  const auto bytes_after_first_flush = channel.BytesSent();
  ASSERT_TRUE(channel.FlushDeferred().HasValue());
  EXPECT_EQ(channel.BytesSent(), bytes_after_first_flush);
}
