#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "network/event_dispatcher.h"
#include "network/message.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/tcp_channel.h"

using namespace atlas;

// A simple test message
struct NetTestMsg {
  uint32_t value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{200,
                                  "NetTestMsg",
                                  MessageLengthStyle::kVariable,
                                  -1,
                                  MessageReliability::kReliable,
                                  MessageUrgency::kImmediate};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint32_t>(value); }

  static auto Deserialize(BinaryReader& r) -> Result<NetTestMsg> {
    auto v = r.Read<uint32_t>();
    if (!v) return v.Error();
    return NetTestMsg{*v};
  }
};

// Poll dispatcher until predicate returns true or timeout expires.
// Avoids flaky sleep_for patterns.
template <typename Pred>
static bool poll_until(EventDispatcher& dispatcher, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    dispatcher.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

class NetworkInterfaceTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};
  NetworkInterface ni_{dispatcher_};

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }
};

TEST_F(NetworkInterfaceTest, StartTcpServer) {
  auto result = ni_.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_NE(ni_.TcpAddress().Port(), 0);
}

TEST_F(NetworkInterfaceTest, StartUdp) {
  auto result = ni_.StartUdp(Address("127.0.0.1", 0));
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_NE(ni_.UdpAddress().Port(), 0);
}

TEST_F(NetworkInterfaceTest, TcpConnectAndAccept) {
  auto server_result = ni_.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(server_result.HasValue());

  auto connect_result = ni_.ConnectTcp(ni_.TcpAddress());
  ASSERT_TRUE(connect_result.HasValue()) << connect_result.Error().Message();
  EXPECT_NE(*connect_result, nullptr);

  // Poll until the accept event is processed (≥1 channel)
  EXPECT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));
}

TEST_F(NetworkInterfaceTest, TcpMessageDispatch) {
  // Start server NI
  auto server_result = ni_.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(server_result.HasValue());
  auto server_addr = ni_.TcpAddress();

  // Register handler
  std::atomic<bool> received{false};
  std::atomic<uint32_t> received_value{0};
  ni_.InterfaceTable().RegisterTypedHandler<NetTestMsg>(
      [&](const Address&, Channel*, const NetTestMsg& msg) {
        received_value.store(msg.value, std::memory_order_relaxed);
        received.store(true, std::memory_order_release);
      });

  // Create a client socket and connect
  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  (void)client_sock->Connect(server_addr);

  // Poll until accept event processed (channel appears on server side)
  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  // Build and send a framed message from client
  Bundle bundle;
  bundle.AddMessage(NetTestMsg{42});
  auto payload = bundle.Finalize();

  // TCP frame: [uint32 length LE][payload]
  uint32_t frame_len = endian::ToLittle(static_cast<uint32_t>(payload.size()));
  std::vector<std::byte> frame;
  auto* len_bytes = reinterpret_cast<const std::byte*>(&frame_len);
  frame.insert(frame.end(), len_bytes, len_bytes + 4);
  frame.insert(frame.end(), payload.begin(), payload.end());

  auto sent = client_sock->Send(frame);
  ASSERT_TRUE(sent.HasValue()) << sent.Error().Message();

  // Poll until the message handler fires
  ASSERT_TRUE(poll_until(dispatcher_, [&] { return received.load(std::memory_order_acquire); }));

  EXPECT_EQ(received_value.load(), 42u);
}

TEST_F(NetworkInterfaceTest, TcpReadableCallbackDrainsAllBufferedFrames) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  constexpr uint32_t kFrameCount = 160;
  std::atomic<uint32_t> received_count{0};
  std::atomic<uint32_t> last_value{0};

  ni_.InterfaceTable().RegisterTypedHandler<NetTestMsg>(
      [&](const Address&, Channel*, const NetTestMsg& msg) {
        last_value.store(msg.value, std::memory_order_relaxed);
        received_count.fetch_add(1, std::memory_order_relaxed);
      });

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  auto connect_result = client_sock->Connect(ni_.TcpAddress());
  ASSERT_TRUE(connect_result.HasValue() || connect_result.Error().Code() == ErrorCode::kWouldBlock);

  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  std::vector<std::byte> frames;
  frames.reserve(kFrameCount * 16);

  for (uint32_t i = 0; i < kFrameCount; ++i) {
    Bundle bundle;
    bundle.AddMessage(NetTestMsg{i});
    auto payload = bundle.Finalize();

    uint32_t frame_len = endian::ToLittle(static_cast<uint32_t>(payload.size()));
    auto* len_bytes = reinterpret_cast<const std::byte*>(&frame_len);
    frames.insert(frames.end(), len_bytes, len_bytes + sizeof(frame_len));
    frames.insert(frames.end(), payload.begin(), payload.end());
  }

  std::size_t sent_total = 0;
  while (sent_total < frames.size()) {
    auto sent = client_sock->Send(
        std::span<const std::byte>(frames.data() + sent_total, frames.size() - sent_total));
    ASSERT_TRUE(sent.HasValue()) << sent.Error().Message();
    ASSERT_GT(*sent, 0u);
    sent_total += *sent;
  }

  ASSERT_TRUE(poll_until(
      dispatcher_, [&] { return received_count.load(std::memory_order_relaxed) == kFrameCount; },
      std::chrono::milliseconds(1000)));

  EXPECT_EQ(received_count.load(std::memory_order_relaxed), kFrameCount);
  EXPECT_EQ(last_value.load(std::memory_order_relaxed), kFrameCount - 1);
}

TEST_F(NetworkInterfaceTest, FindChannel) {
  EXPECT_EQ(ni_.FindChannel(Address("1.2.3.4", 5)), nullptr);
  EXPECT_EQ(ni_.ChannelCount(), 0u);
}

TEST_F(NetworkInterfaceTest, FindChannelById) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  auto connect_result = client_sock->Connect(ni_.TcpAddress());
  ASSERT_TRUE(connect_result.HasValue() || connect_result.Error().Code() == ErrorCode::kWouldBlock);

  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  auto client_addr = client_sock->LocalAddress();
  ASSERT_TRUE(client_addr.HasValue()) << client_addr.Error().Message();

  auto* channel = ni_.FindChannel(*client_addr);
  ASSERT_NE(channel, nullptr);
  EXPECT_EQ(ni_.FindChannel(channel->ChannelId()), channel);
}

TEST_F(NetworkInterfaceTest, PrepareForShutdown) {
  auto result = ni_.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(result.HasValue());

  ni_.PrepareForShutdown();
  // After shutdown, connect should fail or be rejected
  // (channels are condemned)
}

TEST_F(NetworkInterfaceTest, RateLimitRejectsExcess) {
  ni_.SetRateLimit(2);  // max 2 packets per second per IP

  auto result = ni_.StartUdp(Address("127.0.0.1", 0));
  ASSERT_TRUE(result.HasValue());
  auto udp_addr = ni_.UdpAddress();

  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());

  // Burst 10 packets instantly — rate limiter should cap accepted channels at 2
  std::array<std::byte, 4> data{};
  for (int i = 0; i < 10; ++i) {
    (void)sender->SendTo(data, udp_addr);
  }

  // Drain the receive buffer
  poll_until(dispatcher_, [&] { return false; }, std::chrono::milliseconds(50));

  // channel_count reflects accepted UDP "sessions" — must not exceed rate limit
  EXPECT_LE(ni_.ChannelCount(), 2u);
}

// With the per-poll loop now bounded by wall time (kReadableCallbackBudget)
// rather than a fixed datagram count, we can no longer assert "the burst left
// some packets in the OS buffer for the next poll". What we still guarantee
// — and what this test pins — is that while rate-limiting is active, only the
// first admitted source dispatches; lifting the limit then unblocks the path
// for fresh traffic.
TEST_F(NetworkInterfaceTest, UdpRateLimitOnlyAdmitsFirstSourcePerPoll) {
  ASSERT_TRUE(ni_.StartUdp(Address("127.0.0.1", 0)).HasValue());

  std::atomic<uint32_t> received_count{0};
  ni_.InterfaceTable().RegisterTypedHandler<NetTestMsg>(
      [&](const Address&, Channel*, const NetTestMsg&) {
        received_count.fetch_add(1, std::memory_order_relaxed);
      });

  ni_.SetRateLimit(1);

  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());
  ASSERT_TRUE(sender->Bind(Address("127.0.0.1", 0)).HasValue());

  constexpr uint32_t kDatagramCount = 1500;  // intentionally above current per-poll budget
  for (uint32_t i = 0; i < kDatagramCount; ++i) {
    Bundle bundle;
    bundle.AddMessage(NetTestMsg{i});
    auto payload = bundle.Finalize();

    // WSL2 loopback throttles bursts via skb-overhead-bounded queues much
    // earlier than native Linux; retry on kWouldBlock instead of failing.
    Result<size_t> sent = sender->SendTo(payload, ni_.UdpAddress());
    while (!sent && sent.Error().Code() == ErrorCode::kWouldBlock) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      sent = sender->SendTo(payload, ni_.UdpAddress());
    }
    ASSERT_TRUE(sent.HasValue()) << sent.Error().Message();
    ASSERT_EQ(*sent, payload.size());
  }

  // A single ProcessOnce should not dispatch more than one packet while the
  // rate limiter is active — all received datagrams consume read budget but
  // only the first IP is admitted.
  dispatcher_.ProcessOnce();
  EXPECT_EQ(received_count.load(std::memory_order_relaxed), 1u);

  // After lifting the rate limit, fresh traffic must reach the handler.
  // (The burst above may have drained the OS buffer via the time-based read
  // loop — we send a new packet to verify the path is unblocked.)
  ni_.SetRateLimit(0);
  Bundle extra;
  extra.AddMessage(NetTestMsg{9999u});
  auto extra_payload = extra.Finalize();
  ASSERT_TRUE(sender->SendTo(extra_payload, ni_.UdpAddress()).HasValue());

  ASSERT_TRUE(poll_until(
      dispatcher_, [&] { return received_count.load(std::memory_order_relaxed) > 1u; },
      std::chrono::milliseconds(500)));
}

TEST_F(NetworkInterfaceTest, TcpRateLimitedAcceptsAlsoConsumePerPollBudget) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  ni_.SetRateLimit(1);

  constexpr std::size_t kClientCount = 192;  // intentionally above the current accept budget
  std::vector<Socket> clients;
  clients.reserve(kClientCount);

  for (std::size_t i = 0; i < kClientCount; ++i) {
    auto sock = Socket::CreateTcp();
    ASSERT_TRUE(sock.HasValue()) << sock.Error().Message();

    auto connect_result = sock->Connect(ni_.TcpAddress());
    ASSERT_TRUE(connect_result.HasValue() ||
                connect_result.Error().Code() == ErrorCode::kWouldBlock ||
                connect_result.Error().Code() == ErrorCode::kConnectionRefused)
        << connect_result.Error().Message();

    clients.push_back(std::move(*sock));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  dispatcher_.ProcessOnce();
  EXPECT_EQ(ni_.ChannelCount(), 1u);

  ni_.SetRateLimit(0);

  ASSERT_TRUE(poll_until(
      dispatcher_, [&] { return ni_.ChannelCount() > 1u; }, std::chrono::milliseconds(500)));
}

// ============================================================================
// Shutdown: connect_tcp must fail after prepare_for_shutdown
// ============================================================================

TEST_F(NetworkInterfaceTest, ConnectAfterShutdownReturnsError) {
  auto result = ni_.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(result.HasValue());

  ni_.PrepareForShutdown();

  auto conn = ni_.ConnectTcp(ni_.TcpAddress());
  EXPECT_FALSE(conn.HasValue());
}

// ============================================================================
// Channel lifecycle: condemn removes channel, count returns to 0
// ============================================================================

TEST_F(NetworkInterfaceTest, ChannelCountAfterDisconnect) {
  auto server_result = ni_.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(server_result.HasValue());

  auto conn = ni_.ConnectTcp(ni_.TcpAddress());
  ASSERT_TRUE(conn.HasValue());

  // Wait for accept
  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  // Calling PrepareForShutdown condemns all channels
  ni_.PrepareForShutdown();

  // After draining, condemned channels should be cleaned up
  poll_until(dispatcher_, [&] { return false; }, std::chrono::milliseconds(100));

  EXPECT_EQ(ni_.ChannelCount(), 0u);
}

TEST_F(NetworkInterfaceTest, DisconnectCallbackRunsAfterTcpProtocolDisconnect) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  std::atomic<bool> callback_seen{false};
  std::atomic<ChannelId> callback_channel_id{kInvalidChannelId};
  std::atomic<int> callback_state{-1};
  std::atomic<bool> callback_removed_from_active_set{false};

  ni_.SetDisconnectCallback([&](Channel& ch) {
    callback_channel_id.store(ch.ChannelId(), std::memory_order_relaxed);
    callback_state.store(static_cast<int>(ch.State()), std::memory_order_relaxed);
    callback_removed_from_active_set.store(ni_.FindChannel(ch.RemoteAddress()) == nullptr &&
                                               ni_.FindChannel(ch.ChannelId()) == nullptr &&
                                               ni_.ChannelCount() == 0u,
                                           std::memory_order_relaxed);
    callback_seen.store(true, std::memory_order_release);
  });

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  auto connect_result = client_sock->Connect(ni_.TcpAddress());
  ASSERT_TRUE(connect_result.HasValue() || connect_result.Error().Code() == ErrorCode::kWouldBlock);

  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  auto client_addr = client_sock->LocalAddress();
  ASSERT_TRUE(client_addr.HasValue()) << client_addr.Error().Message();

  auto* channel = ni_.FindChannel(*client_addr);
  ASSERT_NE(channel, nullptr);
  const auto expected_channel_id = channel->ChannelId();

  const uint32_t oversized_frame_len = endian::ToLittle(static_cast<uint32_t>(kMaxBundleSize + 1));
  auto* len_bytes = reinterpret_cast<const std::byte*>(&oversized_frame_len);
  auto send_result =
      client_sock->Send(std::span<const std::byte>(len_bytes, sizeof(oversized_frame_len)));
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();
  ASSERT_EQ(*send_result, sizeof(oversized_frame_len));

  ASSERT_TRUE(poll_until(
      dispatcher_, [&] { return callback_seen.load(std::memory_order_acquire); },
      std::chrono::milliseconds(1000)));

  EXPECT_EQ(callback_channel_id.load(std::memory_order_relaxed), expected_channel_id);
  EXPECT_EQ(callback_state.load(std::memory_order_relaxed),
            static_cast<int>(ChannelState::kCondemned));
  EXPECT_TRUE(callback_removed_from_active_set.load(std::memory_order_relaxed));
  EXPECT_EQ(ni_.ChannelCount(), 0u);
}

TEST_F(NetworkInterfaceTest, PrepareForShutdownDoesNotInvokeDisconnectCallback) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  std::atomic<uint32_t> disconnect_callbacks{0};
  ni_.SetDisconnectCallback(
      [&](Channel&) { disconnect_callbacks.fetch_add(1, std::memory_order_relaxed); });

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  auto connect_result = client_sock->Connect(ni_.TcpAddress());
  ASSERT_TRUE(connect_result.HasValue() || connect_result.Error().Code() == ErrorCode::kWouldBlock);

  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  ni_.PrepareForShutdown();
  poll_until(dispatcher_, [&] { return false; }, std::chrono::milliseconds(20));

  EXPECT_EQ(disconnect_callbacks.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(ni_.ChannelCount(), 0u);
}

TEST_F(NetworkInterfaceTest, CondemnedTcpChannelReclaimsExpandedRecvBufferImmediately) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  auto connect_result = client_sock->Connect(ni_.TcpAddress());
  ASSERT_TRUE(connect_result.HasValue() || connect_result.Error().Code() == ErrorCode::kWouldBlock);

  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  auto client_addr = client_sock->LocalAddress();
  ASSERT_TRUE(client_addr.HasValue()) << client_addr.Error().Message();

  auto* channel = dynamic_cast<TcpChannel*>(ni_.FindChannel(*client_addr));
  ASSERT_NE(channel, nullptr);
  EXPECT_EQ(channel->RecvBufferCapacity(), TcpChannel::kInitialRecvBufferSize);

  constexpr std::size_t kFrameLength = 48 * 1024;
  constexpr std::size_t kPartialPayload = 24 * 1024;
  uint32_t frame_len_le = endian::ToLittle(static_cast<uint32_t>(kFrameLength));

  std::vector<std::byte> partial_frame;
  partial_frame.reserve(sizeof(frame_len_le) + kPartialPayload);
  auto* len_bytes = reinterpret_cast<const std::byte*>(&frame_len_le);
  partial_frame.insert(partial_frame.end(), len_bytes, len_bytes + sizeof(frame_len_le));
  partial_frame.insert(partial_frame.end(), kPartialPayload, std::byte{'x'});

  std::size_t sent_total = 0;
  while (sent_total < partial_frame.size()) {
    auto sent = client_sock->Send(std::span<const std::byte>(partial_frame.data() + sent_total,
                                                             partial_frame.size() - sent_total));
    ASSERT_TRUE(sent.HasValue()) << sent.Error().Message();
    ASSERT_GT(*sent, 0u);
    sent_total += *sent;
  }

  ASSERT_TRUE(poll_until(dispatcher_, [&] {
    return channel->RecvBufferCapacity() > TcpChannel::kInitialRecvBufferSize &&
           channel->RecvBufferSize() > 0;
  }));

  ni_.PrepareForShutdown();

  EXPECT_EQ(ni_.ChannelCount(), 0u);
  EXPECT_EQ(channel->RecvBufferCapacity(), TcpChannel::kInitialRecvBufferSize);
  EXPECT_EQ(channel->RecvBufferSize(), 0u);
  EXPECT_EQ(channel->WriteBufferCapacity(), TcpChannel::kInitialWriteBufferSize);
  EXPECT_EQ(channel->WriteBufferSize(), 0u);
}

// ============================================================================
// RUDP tests
// ============================================================================

TEST_F(NetworkInterfaceTest, StartRudpServer) {
  auto result = ni_.StartRudpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_NE(ni_.RudpAddress().Port(), 0);
}

TEST_F(NetworkInterfaceTest, ConnectRudpCreatesChannel) {
  // Server NI
  EventDispatcher server_disp{"rudp_server"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_ni(server_disp);

  auto sr = server_ni.StartRudpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(sr.HasValue()) << sr.Error().Message();

  // Client NI connects to server
  auto cr = ni_.ConnectRudp(server_ni.RudpAddress());
  ASSERT_TRUE(cr.HasValue()) << cr.Error().Message();
  EXPECT_NE(*cr, nullptr);
  EXPECT_EQ(ni_.ChannelCount(), 1u);
}

TEST_F(NetworkInterfaceTest, RudpServerAutoCreatesChannelOnFirstDatagram) {
  auto sr = ni_.StartRudpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(sr.HasValue());
  auto server_addr = ni_.RudpAddress();

  // Send a raw datagram to the server from a plain UDP socket
  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());
  Address any(0, 0);
  (void)sender->Bind(any);

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

  (void)sender->SendTo(pkt, server_addr);

  // Dispatcher should accept the datagram and create a channel
  EXPECT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));
}

TEST_F(NetworkInterfaceTest, ConnectRudpIsIdempotent) {
  EventDispatcher server_disp{"rudp_idem"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_ni(server_disp);
  ASSERT_TRUE(server_ni.StartRudpServer(Address("127.0.0.1", 0)).HasValue());

  auto cr1 = ni_.ConnectRudp(server_ni.RudpAddress());
  auto cr2 = ni_.ConnectRudp(server_ni.RudpAddress());
  ASSERT_TRUE(cr1.HasValue());
  ASSERT_TRUE(cr2.HasValue());
  EXPECT_EQ(*cr1, *cr2);  // same channel reused
  EXPECT_EQ(ni_.ChannelCount(), 1u);
}

// Same rationale as UdpRateLimitOnlyAdmitsFirstSourcePerPoll: the time-based
// read loop replaced the count-based exhaustion semantics, so the new
// invariant is "only one source admitted while limited; new source admitted
// after the limit lifts".
TEST_F(NetworkInterfaceTest, RudpRateLimitOnlyAdmitsFirstSourcePerPoll) {
  ASSERT_TRUE(ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  ni_.SetRateLimit(1);

  constexpr uint32_t kDatagramCount = 1500;  // intentionally above the current RUDP budget
  std::vector<Socket> senders;
  senders.reserve(kDatagramCount);

  std::array<std::byte, 9> pkt{};
  pkt[0] = std::byte{rudp::kFlagHasSeq};
  pkt[4] = std::byte{1};

  for (uint32_t i = 0; i < kDatagramCount; ++i) {
    auto sender = Socket::CreateUdp();
    ASSERT_TRUE(sender.HasValue()) << sender.Error().Message();
    ASSERT_TRUE(sender->Bind(Address("127.0.0.1", 0)).HasValue());

    auto sent = sender->SendTo(pkt, ni_.RudpAddress());
    ASSERT_TRUE(sent.HasValue()) << sent.Error().Message();
    ASSERT_EQ(*sent, pkt.size());

    senders.push_back(std::move(*sender));
  }

  dispatcher_.ProcessOnce();
  EXPECT_EQ(ni_.ChannelCount(), 1u);

  // After lifting the rate limit, a fresh datagram from a new source address
  // must create a second channel. The burst above may have drained the OS
  // buffer via the time-based read loop, so we send a new packet here.
  ni_.SetRateLimit(0);
  auto extra_sender = Socket::CreateUdp();
  ASSERT_TRUE(extra_sender.HasValue());
  ASSERT_TRUE(extra_sender->Bind(Address("127.0.0.1", 0)).HasValue());
  std::array<std::byte, 9> extra_pkt{};
  extra_pkt[0] = std::byte{rudp::kFlagHasSeq};
  extra_pkt[4] = std::byte{1};
  ASSERT_TRUE(extra_sender->SendTo(extra_pkt, ni_.RudpAddress()).HasValue());

  ASSERT_TRUE(poll_until(
      dispatcher_, [&] { return ni_.ChannelCount() > 1u; }, std::chrono::milliseconds(500)));
}

TEST_F(NetworkInterfaceTest, RudpServerDefaultProfileKeepsCongestionControl) {
  EventDispatcher server_disp{"rudp_profile_default_server"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_ni(server_disp);
  Channel* accepted = nullptr;
  server_ni.SetAcceptCallback([&accepted](Channel& ch) { accepted = &ch; });
  ASSERT_TRUE(server_ni.StartRudpServer(Address("127.0.0.1", 0)).HasValue());

  auto conn = ni_.ConnectRudp(server_ni.RudpAddress());
  ASSERT_TRUE(conn.HasValue());
  ASSERT_TRUE((*conn)->SendMessage(NetTestMsg{1}).HasValue());

  ASSERT_TRUE(poll_until(server_disp, [&] { return accepted != nullptr; }));

  auto* server_ch = static_cast<ReliableUdpChannel*>(accepted);
  EXPECT_FALSE(server_ch->Nocwnd());
}

TEST_F(NetworkInterfaceTest, RudpServerClusterProfileDisablesCongestionControl) {
  EventDispatcher server_disp{"rudp_profile_cluster_server"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_ni(server_disp);
  Channel* accepted = nullptr;
  server_ni.SetAcceptCallback([&accepted](Channel& ch) { accepted = &ch; });
  ASSERT_TRUE(
      server_ni.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::ClusterRudpProfile())
          .HasValue());

  auto conn = ni_.ConnectRudp(server_ni.RudpAddress());
  ASSERT_TRUE(conn.HasValue());
  ASSERT_TRUE((*conn)->SendMessage(NetTestMsg{1}).HasValue());

  ASSERT_TRUE(poll_until(server_disp, [&] { return accepted != nullptr; }));

  auto* server_ch = static_cast<ReliableUdpChannel*>(accepted);
  EXPECT_TRUE(server_ch->Nocwnd());
  EXPECT_GT(server_ch->EffectiveWindow(), 1u);
}

// ============================================================================
// Condemned channel lifecycle
// ============================================================================

// TCP has no app-level reliability state — the condemned-channels list
// drops it on the next ProcessCondemnedChannels tick (sub-tick latency).
TEST_F(NetworkInterfaceTest, CondemnedTcpChannelDropsOnNextTick) {
  ASSERT_TRUE(ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  auto client_sock = Socket::CreateTcp();
  ASSERT_TRUE(client_sock.HasValue());
  auto cr = client_sock->Connect(ni_.TcpAddress());
  ASSERT_TRUE(cr.HasValue() || cr.Error().Code() == ErrorCode::kWouldBlock);

  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.ChannelCount() >= 1u; }));

  ni_.PrepareForShutdown();
  EXPECT_EQ(ni_.ChannelCount(), 0u);
  EXPECT_EQ(ni_.CondemnedChannelCount(), 1u);

  // One DoTask sweep is enough — TCP has no unacked.
  ASSERT_TRUE(poll_until(dispatcher_, [&] { return ni_.CondemnedChannelCount() == 0u; }));
}

// RUDP with in-flight reliable: condemned channel survives until the
// peer's ACK drains unacked_, then drops on the following sweep. Tail
// ACK datagrams from the peer route to the condemned channel via
// condemned_rudp_by_addr_ (PrepareForShutdown gates new-channel spawn,
// so the only way the ACK can be processed is through that index).
TEST_F(NetworkInterfaceTest, CondemnedRudpDrainsAfterPeerAck) {
  EventDispatcher server_disp{"rudp_drain_server"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_ni(server_disp);
  ASSERT_TRUE(
      server_ni.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::ClusterRudpProfile())
          .HasValue());
  server_ni.InterfaceTable().RegisterTypedHandler<NetTestMsg>(
      [](const Address&, Channel*, const NetTestMsg&) {});

  auto cr = ni_.ConnectRudp(server_ni.RudpAddress());
  ASSERT_TRUE(cr.HasValue());
  auto* client_ch = *cr;
  ASSERT_TRUE(client_ch->SendMessage(NetTestMsg{1}).HasValue());
  EXPECT_GT(client_ch->UnackedCount(), 0u);

  // Sanity: confirm a normal ACK round-trip works in this setup.
  {
    auto ack_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < ack_deadline && client_ch->UnackedCount() > 0) {
      server_disp.ProcessOnce();
      dispatcher_.ProcessOnce();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(client_ch->UnackedCount(), 0u) << "ACK round-trip pre-condition failed";
  }

  // Now stage a NEW unacked packet, then condemn — the condemned-list
  // path is what we actually want to exercise.
  ASSERT_TRUE(client_ch->SendMessage(NetTestMsg{2}).HasValue());
  EXPECT_GT(client_ch->UnackedCount(), 0u);
  ni_.PrepareForShutdown();
  EXPECT_EQ(ni_.ChannelCount(), 0u);
  EXPECT_EQ(ni_.CondemnedChannelCount(), 1u);
  EXPECT_GT(client_ch->UnackedCount(), 0u) << "Unacked must persist across Condemn()";

  // Drive both dispatchers; the server's ACK comes back, lands on the
  // condemned channel via condemned_rudp_by_addr_, drains unacked, and
  // the next sweep destroys the channel. Allow a generous margin —
  // server's delayed ACK timer is 10ms.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (std::chrono::steady_clock::now() < deadline && ni_.CondemnedChannelCount() > 0) {
    server_disp.ProcessOnce();
    dispatcher_.ProcessOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(ni_.CondemnedChannelCount(), 0u);
}
