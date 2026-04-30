// Integration test: Bidirectional RUDP messaging between two NetworkInterface
// instances, exercising the full NetworkInterface → ReliableUdpChannel path.
//
// Scenarios:
//   1. Client connects to server via RUDP; sends a message; server receives it.
//   2. Server sends a reply back; client receives it (full round-trip).
//   3. Multiple messages in one direction are received in order.
//   4. Large (fragmented) payload is reassembled correctly end-to-end.
//   5. connect_rudp_nocwnd creates a channel with congestion control disabled.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "network/event_dispatcher.h"
#include "network/message.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"

using namespace atlas;

// ============================================================================
// Message definitions
// ============================================================================

struct RudpIntMsg {
  uint32_t seq{0};
  std::string text;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{500, "RudpIntMsg", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(seq);
    w.WriteString(text);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RudpIntMsg> {
    RudpIntMsg msg;
    auto s = r.Read<uint32_t>();
    if (!s) return s.Error();
    msg.seq = *s;
    auto t = r.ReadString();
    if (!t) return t.Error();
    msg.text = std::move(*t);
    return msg;
  }
};

struct RudpLargeMsg {
  std::vector<uint8_t> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{501, "RudpLargeMsg", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(static_cast<uint32_t>(payload.size()));
    for (auto b : payload) w.Write<uint8_t>(b);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RudpLargeMsg> {
    auto sz = r.Read<uint32_t>();
    if (!sz) return sz.Error();
    RudpLargeMsg msg;
    msg.payload.resize(*sz);
    for (uint32_t i = 0; i < *sz; ++i) {
      auto b = r.Read<uint8_t>();
      if (!b) return b.Error();
      msg.payload[i] = *b;
    }
    return msg;
  }
};

// ============================================================================
// Helpers
// ============================================================================

template <typename Pred>
static bool poll_both_until(NetworkInterface& a_ni, EventDispatcher& a, NetworkInterface& b_ni,
                            EventDispatcher& b, Pred pred,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a_ni.FlushDirtySendChannels();
    b_ni.FlushDirtySendChannels();
    a.ProcessOnce();
    b.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// ============================================================================
// Fixture
// ============================================================================

class RudpTwoNiTest : public ::testing::Test {
 protected:
  EventDispatcher server_disp_{"server"};
  EventDispatcher client_disp_{"client"};
  NetworkInterface server_ni_{server_disp_};
  NetworkInterface client_ni_{client_disp_};

  void SetUp() override {
    server_disp_.SetMaxPollWait(Milliseconds(1));
    client_disp_.SetMaxPollWait(Milliseconds(1));
  }
};

// ============================================================================
// Test 1: Client sends message → server receives it
// ============================================================================

TEST_F(RudpTwoNiTest, ClientToServerOneMessage) {
  ASSERT_TRUE(server_ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  auto server_addr = server_ni_.RudpAddress();

  std::atomic<bool> received{false};
  std::atomic<uint32_t> received_seq{0};

  server_ni_.InterfaceTable().RegisterTypedHandler<RudpIntMsg>(
      [&](const Address&, Channel*, const RudpIntMsg& msg) {
        received_seq.store(msg.seq, std::memory_order_relaxed);
        received.store(true, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectRudp(server_addr);
  ASSERT_TRUE(conn.HasValue()) << conn.Error().Message();

  auto* ch = *conn;
  auto send_result = ch->SendMessage(RudpIntMsg{42, "hello"});
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();

  ASSERT_TRUE(poll_both_until(server_ni_, server_disp_, client_ni_, client_disp_, [&] {
    return received.load(std::memory_order_acquire);
  })) << "Server did not receive the RUDP message";

  EXPECT_EQ(received_seq.load(), 42u);
}

// ============================================================================
// Test 2: Full round-trip (client → server → client)
// ============================================================================

TEST_F(RudpTwoNiTest, RoundTripRequestReply) {
  ASSERT_TRUE(server_ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  auto server_addr = server_ni_.RudpAddress();

  // Server echoes the message back using the incoming channel.
  server_ni_.InterfaceTable().RegisterTypedHandler<RudpIntMsg>(
      [&](const Address&, Channel* ch, const RudpIntMsg& msg) {
        if (ch) (void)ch->SendMessage(RudpIntMsg{msg.seq + 1000, "echo:" + msg.text});
      });

  std::atomic<bool> reply_received{false};
  std::atomic<uint32_t> reply_seq{0};

  client_ni_.InterfaceTable().RegisterTypedHandler<RudpIntMsg>(
      [&](const Address&, Channel*, const RudpIntMsg& msg) {
        reply_seq.store(msg.seq, std::memory_order_relaxed);
        reply_received.store(true, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectRudp(server_addr);
  ASSERT_TRUE(conn.HasValue());

  // Wait for the server to create a reverse channel (first datagram triggers it).
  (void)(*conn)->SendMessage(RudpIntMsg{7, "ping"});

  ASSERT_TRUE(poll_both_until(
      server_ni_, server_disp_, client_ni_, client_disp_,
      [&] { return server_ni_.ChannelCount() >= 1u; }, std::chrono::milliseconds(1000)));

  // Allow the round-trip to complete.
  ASSERT_TRUE(poll_both_until(
      server_ni_, server_disp_, client_ni_, client_disp_,
      [&] { return reply_received.load(std::memory_order_acquire); },
      std::chrono::milliseconds(2000)))
      << "Client did not receive the echo reply";

  EXPECT_EQ(reply_seq.load(), 1007u);  // 7 + 1000
}

// ============================================================================
// Test 3: Multiple messages received in order
// ============================================================================

TEST_F(RudpTwoNiTest, MultipleMessagesInOrder) {
  ASSERT_TRUE(server_ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());

  std::vector<uint32_t> received_seqs;
  std::atomic<int> count{0};
  constexpr int kCount = 5;

  server_ni_.InterfaceTable().RegisterTypedHandler<RudpIntMsg>(
      [&](const Address&, Channel*, const RudpIntMsg& msg) {
        received_seqs.push_back(msg.seq);
        count.fetch_add(1, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectRudp(server_ni_.RudpAddress());
  ASSERT_TRUE(conn.HasValue());

  auto* ch = *conn;
  // Disable congestion window so all messages are sent without blocking.
  ch->SetNocwnd(true);

  for (uint32_t i = 0; i < kCount; ++i) {
    (void)ch->SendMessage(RudpIntMsg{i, "msg"});
  }

  ASSERT_TRUE(poll_both_until(
      server_ni_, server_disp_, client_ni_, client_disp_,
      [&] { return count.load(std::memory_order_acquire) >= kCount; },
      std::chrono::milliseconds(2000)));

  ASSERT_EQ(static_cast<int>(received_seqs.size()), kCount);
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(received_seqs[i], static_cast<uint32_t>(i)) << "Order mismatch at index " << i;
  }
}

// ============================================================================
// Test 4: Large (fragmented) payload reassembled end-to-end
// ============================================================================

TEST_F(RudpTwoNiTest, LargePayloadFragmentedAndReassembled) {
  ASSERT_TRUE(server_ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());

  constexpr uint32_t kPayloadSize = 8000;

  std::atomic<bool> received{false};
  std::vector<uint8_t> received_payload;

  server_ni_.InterfaceTable().RegisterTypedHandler<RudpLargeMsg>(
      [&](const Address&, Channel*, const RudpLargeMsg& msg) {
        received_payload = msg.payload;
        received.store(true, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectRudp(server_ni_.RudpAddress());
  ASSERT_TRUE(conn.HasValue());

  auto* ch = *conn;
  // Disable cwnd so all fragments are sent in one burst.
  ch->SetNocwnd(true);

  RudpLargeMsg big;
  big.payload.resize(kPayloadSize);
  for (uint32_t i = 0; i < kPayloadSize; ++i) big.payload[i] = static_cast<uint8_t>(i % 251);

  auto send_result = ch->SendMessage(big);
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();

  ASSERT_TRUE(poll_both_until(
      server_ni_, server_disp_, client_ni_, client_disp_,
      [&] { return received.load(std::memory_order_acquire); }, std::chrono::milliseconds(3000)))
      << "Large payload was never reassembled on the server";

  ASSERT_EQ(received_payload.size(), kPayloadSize);
  for (uint32_t i = 0; i < kPayloadSize; ++i) {
    EXPECT_EQ(received_payload[i], static_cast<uint8_t>(i % 251))
        << "Payload mismatch at byte " << i;
  }
}

// ============================================================================
// Test 5: connect_rudp_nocwnd creates channel with nocwnd=true
// ============================================================================

TEST_F(RudpTwoNiTest, ConnectRudpNocwndCreatesNocwndChannel) {
  ASSERT_TRUE(server_ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());

  auto conn = client_ni_.ConnectRudpNocwnd(server_ni_.RudpAddress());
  ASSERT_TRUE(conn.HasValue()) << conn.Error().Message();

  auto* rudp_ch = dynamic_cast<ReliableUdpChannel*>(*conn);
  ASSERT_NE(rudp_ch, nullptr);
  EXPECT_TRUE(rudp_ch->Nocwnd());
  // With nocwnd the effective window equals the full send window.
  EXPECT_GT(rudp_ch->EffectiveWindow(), 1u);
}

// ============================================================================
// Test 6: Server channel count increases on first client datagram
// ============================================================================

TEST_F(RudpTwoNiTest, ServerChannelCountIncrementsOnConnect) {
  ASSERT_TRUE(server_ni_.StartRudpServer(Address("127.0.0.1", 0)).HasValue());
  EXPECT_EQ(server_ni_.ChannelCount(), 0u);

  server_ni_.InterfaceTable().RegisterTypedHandler<RudpIntMsg>(
      [](const Address&, Channel*, const RudpIntMsg&) {});

  auto conn = client_ni_.ConnectRudp(server_ni_.RudpAddress());
  ASSERT_TRUE(conn.HasValue());
  (void)(*conn)->SendMessage(RudpIntMsg{0, "probe"});

  ASSERT_TRUE(poll_both_until(server_ni_, server_disp_, client_ni_, client_disp_, [&] {
    return server_ni_.ChannelCount() >= 1u;
  })) << "Server channel count did not increase after first RUDP datagram";
}
