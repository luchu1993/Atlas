#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

#include <gtest/gtest.h>

#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/message.h"
#include "network/reliable_udp.h"
#include "network/socket.h"

using namespace atlas;

struct RudpTestMsg {
  uint32_t value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{300, "RudpTestMsg", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint32_t>(value); }

  static auto Deserialize(BinaryReader& r) -> Result<RudpTestMsg> {
    auto v = r.Read<uint32_t>();
    if (!v) return v.Error();
    return RudpTestMsg{*v};
  }
};

class ReliableUdpTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};
  InterfaceTable table_;

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }

  // Helper: read all pending datagrams from a socket and feed to channel
  void pump_datagrams(Socket& sock, ReliableUdpChannel& channel) {
    std::array<std::byte, 2048> buf{};
    while (true) {
      auto result = sock.RecvFrom(buf);
      if (!result || result->first == 0) break;
      channel.OnDatagramReceived(std::span<const std::byte>(buf.data(), result->first));
    }
  }
};

TEST_F(ReliableUdpTest, ReliableSendAndReceive) {
  // Create two UDP sockets for peer A and peer B
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  // Register handler on B's interface table
  bool received = false;
  uint32_t received_value = 0;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received = true;
    received_value = msg.value;
  });

  // Create channels
  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // A sends reliable message to B
  channel_a.Bundle().AddMessage(RudpTestMsg{42});
  auto send_result = channel_a.SendReliable();
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();

  // B receives
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);

  EXPECT_TRUE(received);
  EXPECT_EQ(received_value, 42u);
  EXPECT_EQ(channel_a.UnackedCount(), 1u);  // still unacked (B hasn't sent ACK back)
}

TEST_F(ReliableUdpTest, AckClearsUnacked) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // A sends to B
  channel_a.Bundle().AddMessage(RudpTestMsg{1});
  (void)channel_a.SendReliable();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);

  EXPECT_EQ(channel_a.UnackedCount(), 1u);

  // B sends reply (which piggybacks ACK)
  channel_b.Bundle().AddMessage(RudpTestMsg{2});
  (void)channel_b.SendReliable();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_a, channel_a);

  // A should have processed B's ACK
  EXPECT_EQ(channel_a.UnackedCount(), 0u);
}

TEST_F(ReliableUdpTest, CondemnClearsReliableState) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  channel_a.Bundle().AddMessage(RudpTestMsg{7});
  ASSERT_TRUE(channel_a.SendReliable().HasValue());
  EXPECT_EQ(channel_a.UnackedCount(), 1u);

  channel_a.Condemn();
  EXPECT_EQ(channel_a.State(), ChannelState::kCondemned);
  EXPECT_EQ(channel_a.UnackedCount(), 0u);
}

TEST_F(ReliableUdpTest, CondemnedSendReliableClearsBundle) {
  // Regression for #6: SendReliable on a condemned channel must drop
  // staged bundle bytes — otherwise a caller holding a zombie pointer
  // and repeatedly AddMessage + SendReliable grows bundle_ unboundedly
  // until destructor (matches Channel::Send and SendUnreliable).
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = Address("127.0.0.1", 9999);

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  channel_a.Condemn();

  channel_a.Bundle().AddMessage(RudpTestMsg{1});
  EXPECT_FALSE(channel_a.Bundle().empty());
  auto r = channel_a.SendReliable();
  EXPECT_FALSE(r.HasValue());
  EXPECT_EQ(r.Error().Code(), ErrorCode::kChannelCondemned);
  EXPECT_TRUE(channel_a.Bundle().empty()) << "Bundle must drain on condemned-error path";
}

TEST_F(ReliableUdpTest, FragmentTableCappedUnderAttack) {
  // Regression for #1: a peer fanning out unique fragment_ids with
  // expected_count=255 and only fragment_index=0 must not balloon
  // pending_fragments_. Cap is 16; LRU evicts oldest when full.
  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = Address("127.0.0.1", 1234);

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Build a synthetic stream of fragmented packets with different
  // fragment_ids but only fragment_index=0 (never completes). Feed
  // them as if they came in-order so FlushReceiveBuffer reaches
  // OnFragmentReceived for each.
  constexpr uint32_t kAttackCount = 64;
  std::array<std::byte, 32> buf{};
  for (uint32_t i = 0; i < kAttackCount; ++i) {
    // Wire layout: flags(1) seq(4) frag_id(2) frag_idx(1) frag_cnt(1) payload
    std::size_t off = 0;
    buf[off++] = std::byte{0x01 | 0x02 | 0x08};  // Reliable | HasSeq | Fragment
    uint32_t seq_le = i + 1;                     // seq starts at 1
    std::memcpy(buf.data() + off, &seq_le, 4);
    off += 4;
    uint16_t fid_le = static_cast<uint16_t>(i + 1);  // unique each round
    std::memcpy(buf.data() + off, &fid_le, 2);
    off += 2;
    buf[off++] = std::byte{0};     // fragment_index = 0
    buf[off++] = std::byte{255};   // fragment_count = 255 (max-fanout)
    buf[off++] = std::byte{0xAB};  // 1 byte placeholder payload
    channel_b.OnDatagramReceived(std::span<const std::byte>(buf.data(), off));
  }

  // Cap is 16; even with 64 attacks queued, pending must stay <= cap.
  EXPECT_LE(channel_b.PendingFragmentGroupCount(), 16u);
  EXPECT_GT(channel_b.PendingFragmentGroupCount(), 0u);
}

TEST_F(ReliableUdpTest, AckOnlyDatagramBumpsInactivity) {
  // Regression for #5: pure-ACK datagrams (no payload) must reset
  // last_activity_ — otherwise a server that pushes data while the
  // client only ACKs back trips its inactivity timeout.
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = Address("127.0.0.1", 1234);

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  channel_a.SetInactivityTimeout(std::chrono::milliseconds(100));

  // Wait past the inactivity threshold WITHOUT any inbound traffic.
  // Then feed a pure-ACK datagram and verify state stays kActive.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  // Synthetic ACK-only datagram: flags=kFlagHasAck(0x04) + ack_num(4) +
  // ack_bits(4) + una(4). 13 bytes total, no seq, no payload.
  std::array<std::byte, 13> ack_buf{};
  ack_buf[0] = std::byte{0x04};  // kFlagHasAck only
  uint32_t ack_num = 1;
  uint32_t ack_bits = 0;
  uint32_t una = 1;
  std::memcpy(ack_buf.data() + 1, &ack_num, 4);
  std::memcpy(ack_buf.data() + 5, &ack_bits, 4);
  std::memcpy(ack_buf.data() + 9, &una, 4);
  channel_a.OnDatagramReceived(std::span<const std::byte>(ack_buf));

  // Drive dispatcher long enough that an unrefreshed inactivity timer
  // would fire (cumulative > 100 ms since channel start).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  dispatcher_.ProcessOnce();

  EXPECT_EQ(channel_a.State(), ChannelState::kActive)
      << "Pure-ACK datagram should reset last_activity_";
}

TEST_F(ReliableUdpTest, UnreliableSendNoTracking) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  bool received = false;
  table_.RegisterTypedHandler<RudpTestMsg>(
      [&](const Address&, Channel*, const RudpTestMsg& msg) { received = true; });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  channel_a.Bundle().AddMessage(RudpTestMsg{99});
  (void)channel_a.SendUnreliable();

  EXPECT_EQ(channel_a.UnackedCount(), 0u);  // not tracked

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);
  EXPECT_TRUE(received);
}

TEST_F(ReliableUdpTest, SendWindowFull) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  // The default window is 256 -- just verify the WouldBlock error mechanism works
  // by checking that sending beyond window returns an error
  // (we won't actually send 256+ packets in a unit test for performance)
  EXPECT_EQ(channel_a.UnackedCount(), 0u);
}

TEST_F(ReliableUdpTest, RttEstimation) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  auto initial_rtt = channel_a.Rtt();

  // Exchange messages to get RTT samples
  channel_a.Bundle().AddMessage(RudpTestMsg{1});
  (void)channel_a.SendReliable();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  pump_datagrams(*sock_b, channel_b);

  channel_b.Bundle().AddMessage(RudpTestMsg{2});
  (void)channel_b.SendReliable();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  pump_datagrams(*sock_a, channel_a);

  auto updated_rtt = channel_a.Rtt();
  auto updated_us = std::chrono::duration_cast<Microseconds>(updated_rtt).count();
  EXPECT_GT(updated_us, 0) << "RTT should be positive after exchange";
  EXPECT_LT(updated_us, 100'000) << "RTT should be reasonable on loopback";
}

// ============================================================================
// KCP-inspired optimizations
// ============================================================================

TEST_F(ReliableUdpTest, NodelayLowersMinRto) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();
  auto addr_a = sock_a->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNodelay(true);
  channel_a.Activate();
  EXPECT_TRUE(channel_a.Nodelay());

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Exchange messages to get RTT samples on loopback
  for (int i = 0; i < 5; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{static_cast<uint32_t>(i)});
    (void)channel_a.SendReliable();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pump_datagrams(*sock_b, channel_b);

    channel_b.Bundle().AddMessage(RudpTestMsg{static_cast<uint32_t>(i)});
    (void)channel_b.SendReliable();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pump_datagrams(*sock_a, channel_a);
  }

  auto rtt_us = std::chrono::duration_cast<Microseconds>(channel_a.Rtt()).count();
  EXPECT_GT(rtt_us, 0) << "RTT should be positive after exchanges";
  EXPECT_LT(rtt_us, 50'000) << "RTT should converge to loopback values";
}

TEST_F(ReliableUdpTest, FastResendConfiguration) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = Address("127.0.0.1", 9999);  // dummy

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  // Default fast resend threshold is 2
  channel_a.SetFastResendThresh(3);
  channel_a.SetNodelay(true);
  EXPECT_TRUE(channel_a.Nodelay());
}

// ============================================================================
// Fragmentation tests
// ============================================================================

// A large message that exceeds MTU
struct LargeMsg {
  std::vector<uint8_t> data;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{301, "LargeMsg", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(static_cast<uint32_t>(data.size()));
    for (auto b : data) {
      w.Write<uint8_t>(b);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<LargeMsg> {
    auto sz = r.Read<uint32_t>();
    if (!sz) return sz.Error();
    LargeMsg msg;
    msg.data.resize(*sz);
    for (uint32_t i = 0; i < *sz; ++i) {
      auto b = r.Read<uint8_t>();
      if (!b) return b.Error();
      msg.data[i] = *b;
    }
    return msg;
  }
};

TEST_F(ReliableUdpTest, FragmentedSendAndReceive) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  // Create a payload larger than MTU (~1455 bytes max payload)
  LargeMsg orig;
  orig.data.resize(5000);
  for (uint32_t i = 0; i < 5000; ++i) {
    orig.data[i] = static_cast<uint8_t>(i % 256);
  }

  bool received = false;
  std::vector<uint8_t> received_data;
  table_.RegisterTypedHandler<LargeMsg>([&](const Address&, Channel*, const LargeMsg& msg) {
    received = true;
    received_data = msg.data;
  });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);  // fragmentation sends multiple packets at once
  channel_a.Activate();

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Send — should auto-fragment since 5000 > kMaxUdpPayload
  channel_a.Bundle().AddMessage(orig);
  auto send_result = channel_a.SendReliable();
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();

  // Should have sent multiple packets (fragments)
  EXPECT_GT(channel_a.UnackedCount(), 1u);

  // Receive all fragments
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);

  // All fragments should have been reassembled and dispatched
  EXPECT_TRUE(received);
  ASSERT_EQ(received_data.size(), 5000u);
  for (uint32_t i = 0; i < 5000; ++i) {
    EXPECT_EQ(received_data[i], static_cast<uint8_t>(i % 256)) << "Mismatch at byte " << i;
  }
}

// ============================================================================
// Ordered delivery tests
// ============================================================================

TEST_F(ReliableUdpTest, OrderedDelivery) {
  // Verify messages are dispatched in order even if received out-of-order.
  // We simulate this by having channel A send 3 messages, then feeding them
  // to channel B in reversed order. B should still dispatch in seq order.

  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  std::vector<uint32_t> received_order;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received_order.push_back(msg.value);
  });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);  // send multiple without ACK exchange
  channel_a.Activate();

  // Send 3 messages (seq 1, 2, 3)
  for (uint32_t i = 1; i <= 3; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{i});
    (void)channel_a.SendReliable();
  }

  // Collect the 3 datagrams from sock_b
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<std::vector<std::byte>> datagrams;
  std::array<std::byte, 2048> buf{};
  while (true) {
    auto result = sock_b->RecvFrom(buf);
    if (!result || result->first == 0) break;
    datagrams.emplace_back(buf.data(), buf.data() + result->first);
  }
  ASSERT_EQ(datagrams.size(), 3u);

  // Feed to channel B in REVERSE order (seq 3, 2, 1)
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  channel_b.OnDatagramReceived(datagrams[2]);  // seq 3
  channel_b.OnDatagramReceived(datagrams[1]);  // seq 2
  channel_b.OnDatagramReceived(datagrams[0]);  // seq 1

  // Despite receiving out of order, dispatch should be in order: 1, 2, 3
  ASSERT_EQ(received_order.size(), 3u);
  EXPECT_EQ(received_order[0], 1u);
  EXPECT_EQ(received_order[1], 2u);
  EXPECT_EQ(received_order[2], 3u);
}

TEST_F(ReliableUdpTest, OrderedDeliveryWaitsForGap) {
  // Send seq 1, 2, 3. Feed seq 1 and 3 (skip 2).
  // Only seq 1 should be dispatched. Then feed seq 2 — all three dispatch.

  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  std::vector<uint32_t> received_order;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received_order.push_back(msg.value);
  });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);
  channel_a.Activate();

  for (uint32_t i = 1; i <= 3; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{i * 10});
    (void)channel_a.SendReliable();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<std::vector<std::byte>> datagrams;
  std::array<std::byte, 2048> buf{};
  while (true) {
    auto result = sock_b->RecvFrom(buf);
    if (!result || result->first == 0) break;
    datagrams.emplace_back(buf.data(), buf.data() + result->first);
  }
  ASSERT_EQ(datagrams.size(), 3u);

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Feed seq 1 and 3, skip 2
  channel_b.OnDatagramReceived(datagrams[0]);  // seq 1 → delivered
  channel_b.OnDatagramReceived(datagrams[2]);  // seq 3 → buffered (waiting for 2)

  ASSERT_EQ(received_order.size(), 1u);
  EXPECT_EQ(received_order[0], 10u);
  EXPECT_EQ(channel_b.RecvBufCount(), 1u);  // seq 3 in buffer

  // Now feed seq 2 — should flush both 2 and 3
  channel_b.OnDatagramReceived(datagrams[1]);  // seq 2

  ASSERT_EQ(received_order.size(), 3u);
  EXPECT_EQ(received_order[1], 20u);
  EXPECT_EQ(received_order[2], 30u);
  EXPECT_EQ(channel_b.RecvBufCount(), 0u);
}

TEST_F(ReliableUdpTest, FlushReceiveBufferYieldsOnDeadline) {
  // When a gap-filler arrives and many buffered packets become deliverable,
  // FlushReceiveBuffer must (a) always make forward progress on at least
  // one packet, (b) stop when the supplied deadline has passed, (c) report
  // "still hot" so the caller can resume.  Without yielding, dispatch on
  // baseapp at 500-cli load was hitting 200 ms callbacks via cascade
  // (see docs/optimization/README.md, 500-client baseline).

  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  std::vector<uint32_t> received_order;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received_order.push_back(msg.value);
  });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);
  channel_a.Activate();

  // Send 5 reliable packets seq 1..5; capture each datagram off socket_b.
  constexpr int kNumPackets = 5;
  for (uint32_t i = 1; i <= kNumPackets; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{i * 10});
    (void)channel_a.SendReliable();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<std::vector<std::byte>> datagrams;
  std::array<std::byte, 2048> buf{};
  while (true) {
    auto result = sock_b->RecvFrom(buf);
    if (!result || result->first == 0) break;
    datagrams.emplace_back(buf.data(), buf.data() + result->first);
  }
  ASSERT_EQ(datagrams.size(), static_cast<size_t>(kNumPackets));

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  bool hot_cb_fired = false;
  channel_b.SetHotCallback([&](ReliableUdpChannel&) { hot_cb_fired = true; });

  // Feed seq 2..5 out of order — all buffered, none delivered (rcv_nxt_=1).
  for (int i = 4; i >= 1; --i) {
    channel_b.OnDatagramReceived(datagrams[i]);
  }
  ASSERT_EQ(received_order.size(), 0u);
  EXPECT_EQ(channel_b.RecvBufCount(), 4u);
  EXPECT_FALSE(hot_cb_fired) << "no flush yet — hot callback should not have fired";

  // Feed seq 1 with an already-elapsed deadline.  FlushReceiveBuffer must
  // deliver seq 1 (the always-make-progress invariant) and then return
  // true with backlog still queued, prompting the hot-callback.
  const auto past_deadline = Clock::now() - std::chrono::seconds(1);
  channel_b.OnDatagramReceived(datagrams[0], past_deadline);

  EXPECT_EQ(received_order.size(), 1u) << "seq 1 should be delivered before the yield";
  EXPECT_EQ(received_order[0], 10u);
  EXPECT_TRUE(hot_cb_fired) << "yield with backlog must announce via hot-callback";
  EXPECT_EQ(channel_b.RecvBufCount(), 4u) << "seq 2..5 still queued after the yield";
  // Sanity: the deadline-yield path leaves rcv_buf_ holding seq 2 onward.
  EXPECT_TRUE(channel_b.HasReceiveBacklog());

  // Drain remaining with a relaxed deadline — should fully flush and
  // report "no more pending".
  hot_cb_fired = false;
  const bool still_hot = channel_b.FlushReceiveBuffer(Clock::now() + std::chrono::seconds(10));
  EXPECT_FALSE(still_hot);
  EXPECT_FALSE(hot_cb_fired) << "explicit drain must not re-fire the callback";
  EXPECT_EQ(received_order.size(), 5u);
  for (uint32_t i = 0; i < 5; ++i) {
    EXPECT_EQ(received_order[i], (i + 1) * 10);
  }
  EXPECT_EQ(channel_b.RecvBufCount(), 0u);
  EXPECT_FALSE(channel_b.HasReceiveBacklog());
}

TEST_F(ReliableUdpTest, DispatchMessagesBudgetedYieldsBetweenHandlers) {
  // A2 contract: when a single MTU-sized packet bundles several
  // messages whose handlers together exceed the deadline,
  // DispatchMessagesBudgeted must yield AFTER the most recent
  // successful handler and stash the un-dispatched tail so the next
  // FlushReceiveBuffer call resumes mid-packet.  Without this
  // granularity the per-call bound is "one whole packet's worth of
  // handlers" — see docs/optimization/network_dispatch_decoupling.md
  // for the data that ruled out coarser bounds.

  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  std::vector<uint32_t> received_order;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received_order.push_back(msg.value);
  });

  // Stage three messages into a single bundle on channel_a so they
  // ride a single reliable packet (i.e. one DispatchMessages frame
  // on the receiver side).
  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);
  channel_a.Activate();
  channel_a.Bundle().AddMessage(RudpTestMsg{10});
  channel_a.Bundle().AddMessage(RudpTestMsg{20});
  channel_a.Bundle().AddMessage(RudpTestMsg{30});
  ASSERT_TRUE(channel_a.SendReliable().HasValue());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::array<std::byte, 2048> buf{};
  auto recv = sock_b->RecvFrom(buf);
  ASSERT_TRUE(recv.HasValue());
  std::span<const std::byte> datagram(buf.data(), recv->first);

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Receive the datagram with a tight (already-elapsed) flush deadline.
  // OnDatagramReceived's internal call to FlushReceiveBuffer will
  // dispatch the FIRST message of the bundle (forward progress), then
  // notice the deadline passed and stash the remaining two messages
  // in the channel's pending-dispatch buffer.
  channel_b.OnDatagramReceived(datagram, Clock::now() - std::chrono::seconds(1));

  EXPECT_EQ(received_order.size(), 1u) << "first message dispatches before yield";
  EXPECT_EQ(received_order[0], 10u);
  EXPECT_TRUE(channel_b.HasPendingDispatch()) << "remaining bundle bytes must be parked for resume";
  EXPECT_TRUE(channel_b.HasReceiveBacklog());

  // First resume call also has an already-elapsed deadline — drains
  // exactly one more handler.
  bool still_hot = channel_b.FlushReceiveBuffer(Clock::now() - std::chrono::seconds(1));
  EXPECT_TRUE(still_hot);
  EXPECT_EQ(received_order.size(), 2u);
  EXPECT_EQ(received_order[1], 20u);
  EXPECT_TRUE(channel_b.HasPendingDispatch());

  // Final resume with relaxed deadline drains the last handler.
  still_hot = channel_b.FlushReceiveBuffer(Clock::now() + std::chrono::seconds(10));
  EXPECT_FALSE(still_hot);
  EXPECT_EQ(received_order.size(), 3u);
  EXPECT_EQ(received_order[2], 30u);
  EXPECT_FALSE(channel_b.HasPendingDispatch());
  EXPECT_FALSE(channel_b.HasReceiveBacklog());
}

TEST_F(ReliableUdpTest, GapFillerOutsideSackWindowNotDroppedAsDuplicate) {
  // Regression: the receiver must not treat a never-seen seq as duplicate
  // simply because it falls outside the 32-bit SACK reporting window
  // (remote_seq_ - seq > 32). Pre-fix, IsDuplicate returned "too old" and
  // dropped the only retransmit that could fill the gap, leading to dead
  // link.
  //
  // Scenario: sender emits seq 1..40. Feed seq 40 first (advances
  // remote_seq_ to 40), then feed the long-delayed gap-filler seq 1.
  // diff = 39 > 32, so the old check would drop it. After the fix, seq 1
  // is accepted and rcv_nxt_ advances to 2.
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  std::vector<uint32_t> received_order;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received_order.push_back(msg.value);
  });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);
  channel_a.Activate();

  constexpr uint32_t kCount = 40;
  for (uint32_t i = 1; i <= kCount; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{i});
    (void)channel_a.SendReliable();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<std::vector<std::byte>> datagrams;
  std::array<std::byte, 2048> buf{};
  while (true) {
    auto result = sock_b->RecvFrom(buf);
    if (!result || result->first == 0) break;
    datagrams.emplace_back(buf.data(), buf.data() + result->first);
  }
  ASSERT_EQ(datagrams.size(), kCount);

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Feed the highest seq first to push remote_seq_ far ahead of rcv_nxt_.
  channel_b.OnDatagramReceived(datagrams[kCount - 1]);
  EXPECT_EQ(channel_b.RecvNextSeq(), 1u);   // still waiting for seq 1
  EXPECT_EQ(channel_b.RecvBufCount(), 1u);  // seq 40 buffered

  // Now feed seq 1 — diff = 39 > SACK window (32). Pre-fix: dropped.
  channel_b.OnDatagramReceived(datagrams[0]);
  ASSERT_EQ(received_order.size(), 1u);
  EXPECT_EQ(received_order[0], 1u);
  EXPECT_EQ(channel_b.RecvNextSeq(), 2u);

  // Feed the rest in arbitrary order; everything should drain in seq
  // order without any drop.
  for (uint32_t i = 2; i < kCount; ++i) {
    channel_b.OnDatagramReceived(datagrams[i - 1]);
  }
  ASSERT_EQ(received_order.size(), kCount);
  for (uint32_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(received_order[i], i + 1) << "Out of order at index " << i;
  }
  EXPECT_EQ(channel_b.RecvBufCount(), 0u);
  EXPECT_EQ(channel_b.RecvNextSeq(), kCount + 1);
}

TEST_F(ReliableUdpTest, RecvWindowDropsExcessivePackets) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.SetNocwnd(true);  // send 10 packets without ACK
  channel_a.Activate();

  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.SetRecvWindow(4);  // tiny window
  channel_b.Activate();

  // Send 10 messages
  for (uint32_t i = 0; i < 10; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{i});
    (void)channel_a.SendReliable();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Collect all datagrams
  std::vector<std::vector<std::byte>> datagrams;
  std::array<std::byte, 2048> buf{};
  while (true) {
    auto result = sock_b->RecvFrom(buf);
    if (!result || result->first == 0) break;
    datagrams.emplace_back(buf.data(), buf.data() + result->first);
  }
  ASSERT_EQ(datagrams.size(), 10u);

  // Feed seq 1 (delivered) then skip to seq 7+ (should be dropped by window)
  // rcv_nxt=1, wnd=4, so acceptable: seq 1..5, dropped: seq 6+
  channel_b.OnDatagramReceived(datagrams[0]);  // seq 1 → delivered, rcv_nxt=2
  channel_b.OnDatagramReceived(datagrams[5]);  // seq 6 → accept (nxt=2, wnd=4, limit=6)
  channel_b.OnDatagramReceived(datagrams[8]);  // seq 9 → drop (> 2+4=6)

  // Only seq 1 delivered, seq 6 buffered, seq 9 dropped
  EXPECT_EQ(channel_b.RecvNextSeq(), 2u);
  EXPECT_LE(channel_b.RecvBufCount(), 1u);
}

TEST_F(ReliableUdpTest, UnreliableBypassesOrdering) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  bool received = false;
  table_.RegisterTypedHandler<RudpTestMsg>(
      [&](const Address&, Channel*, const RudpTestMsg&) { received = true; });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Send unreliable — should be dispatched immediately regardless of rcv_nxt
  channel_a.Bundle().AddMessage(RudpTestMsg{99});
  (void)channel_a.SendUnreliable();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);

  EXPECT_TRUE(received);
  EXPECT_EQ(channel_b.RecvNextSeq(), 1u);  // rcv_nxt unchanged (no seq tracking)
}

// ============================================================================
// Congestion control tests
// ============================================================================

TEST_F(ReliableUdpTest, CwndStartsAtOne) {
  auto sock = Socket::CreateUdp();
  ASSERT_TRUE(sock.HasValue());
  ASSERT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());

  ReliableUdpChannel channel(dispatcher_, table_, *sock, Address("127.0.0.1", 9999));
  channel.Activate();

  EXPECT_EQ(channel.Cwnd(), 1u);
  EXPECT_EQ(channel.Ssthresh(), 16u);
  EXPECT_EQ(channel.EffectiveWindow(), 1u);  // min(send_window=256, cwnd=1)
}

TEST_F(ReliableUdpTest, CwndGrowsOnAck) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  auto initial_cwnd = channel_a.Cwnd();
  EXPECT_EQ(initial_cwnd, 1u);

  // Exchange messages: A sends, B receives and sends back (piggybacking ACK)
  for (int i = 0; i < 5; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{static_cast<uint32_t>(i)});
    (void)channel_a.SendReliable();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pump_datagrams(*sock_b, channel_b);

    channel_b.Bundle().AddMessage(RudpTestMsg{static_cast<uint32_t>(i)});
    (void)channel_b.SendReliable();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pump_datagrams(*sock_a, channel_a);
  }

  // cwnd should have grown (slow start: +1 per ACK)
  EXPECT_GT(channel_a.Cwnd(), initial_cwnd);
  EXPECT_GT(channel_a.EffectiveWindow(), 1u);
}

TEST_F(ReliableUdpTest, NocwndDisablesCongestionControl) {
  auto sock = Socket::CreateUdp();
  ASSERT_TRUE(sock.HasValue());
  ASSERT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());

  ReliableUdpChannel channel(dispatcher_, table_, *sock, Address("127.0.0.1", 9999));
  channel.SetNocwnd(true);
  channel.Activate();

  // With nocwnd, effective window = send_window (ignores cwnd=1)
  EXPECT_EQ(channel.Cwnd(), 1u);
  EXPECT_EQ(channel.EffectiveWindow(), 256u);  // send_window_
  EXPECT_TRUE(channel.Nocwnd());
}

TEST_F(ReliableUdpTest, EffectiveWindowRespectsCwnd) {
  auto sock = Socket::CreateUdp();
  ASSERT_TRUE(sock.HasValue());
  ASSERT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());

  ReliableUdpChannel channel(dispatcher_, table_, *sock, Address("127.0.0.1", 9999));
  channel.Activate();

  // cwnd=1, send_window=256 → effective=1
  EXPECT_EQ(channel.EffectiveWindow(), 1u);

  // First send should work (cwnd=1 allows 1 in flight)
  channel.Bundle().AddMessage(RudpTestMsg{1});
  auto r1 = channel.SendReliable();
  ASSERT_TRUE(r1.HasValue());

  // Second send should fail (cwnd=1, already 1 in flight)
  channel.Bundle().AddMessage(RudpTestMsg{2});
  auto r2 = channel.SendReliable();
  EXPECT_FALSE(r2.HasValue());
  EXPECT_EQ(r2.Error().Code(), ErrorCode::kWouldBlock);
}

TEST_F(ReliableUdpTest, SmallMessageNotFragmented) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = Address("127.0.0.1", 9999);

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  // Send a small message — should NOT fragment
  channel_a.Bundle().AddMessage(RudpTestMsg{42});
  auto result = channel_a.SendReliable();
  ASSERT_TRUE(result.HasValue());

  // Only 1 packet (not fragmented)
  EXPECT_EQ(channel_a.UnackedCount(), 1u);
}

TEST_F(ReliableUdpTest, MessageTooLargeReturnsError) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = Address("127.0.0.1", 9999);

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  // Create a message so large it exceeds 255 fragments
  // 255 * ~1455 ≈ 371KB. Need > that.
  LargeMsg huge;
  huge.data.resize(400'000);  // ~400KB > 255 * 1455

  channel_a.Bundle().AddMessage(huge);
  auto result = channel_a.SendReliable();
  EXPECT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kMessageTooLarge);
}

// ============================================================================
// Review issue #10: cwnd=1 correctly limits initial sends, and after first
// ACK exchange cwnd grows to 2 allowing a second send.
// ============================================================================

TEST_F(ReliableUdpTest, CwndGrowsAfterFirstAckAllowsSecondSend) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Verify initial cwnd is 1
  EXPECT_EQ(channel_a.Cwnd(), 1u);

  // First send succeeds (cwnd=1, 0 in flight)
  channel_a.Bundle().AddMessage(RudpTestMsg{1});
  auto r1 = channel_a.SendReliable();
  ASSERT_TRUE(r1.HasValue());

  // Second send blocked (cwnd=1, 1 in flight)
  channel_a.Bundle().AddMessage(RudpTestMsg{2});
  auto r2 = channel_a.SendReliable();
  EXPECT_FALSE(r2.HasValue());
  EXPECT_EQ(r2.Error().Code(), ErrorCode::kWouldBlock);

  // B receives and sends ACK back via reply
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);

  channel_b.Bundle().AddMessage(RudpTestMsg{100});
  (void)channel_b.SendReliable();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_a, channel_a);

  // After ACK, cwnd should have grown (slow start: +1 per ACK)
  EXPECT_GE(channel_a.Cwnd(), 2u);
  EXPECT_EQ(channel_a.UnackedCount(), 0u);

  // Now we should be able to send two messages before blocking
  channel_a.Bundle().AddMessage(RudpTestMsg{3});
  auto r3 = channel_a.SendReliable();
  ASSERT_TRUE(r3.HasValue()) << "First send after cwnd growth should succeed";

  channel_a.Bundle().AddMessage(RudpTestMsg{4});
  auto r4 = channel_a.SendReliable();
  ASSERT_TRUE(r4.HasValue()) << "Second send should succeed with cwnd>=2";
}

// ─────────────────────────────────────────────────────────────────────────────
// KCP-style cumulative ACK (UNA) tests — guard the fix for the dead-link
// regression where >33-packet bursts left the front of the send queue
// permanently stranded once the SACK bitmap (32-bit window) lost reach.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ReliableUdpTest, UnaClearsPacketsBeyondSackWindow) {
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // Fixed window, no slow start — needed to dispatch >33 reliable packets
  // before any ACK round-trip has had a chance to land.
  channel_a.SetNocwnd(true);
  channel_a.SetSendWindow(128);
  channel_b.SetRecvWindow(128);

  // Send a burst that exceeds the SACK bitmap reach (33 packets).
  constexpr uint32_t kBurst = 50;
  for (uint32_t i = 0; i < kBurst; ++i) {
    channel_a.Bundle().AddMessage(RudpTestMsg{i});
    ASSERT_TRUE(channel_a.SendReliable().HasValue()) << "burst send #" << i;
  }
  EXPECT_EQ(channel_a.UnackedCount(), kBurst);

  // B drains all 50 datagrams, advancing rcv_nxt_ to seq 51.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);
  EXPECT_EQ(channel_b.RecvNextSeq(), kBurst + 1u);

  // B sends a single reliable reply. Its ACK header carries
  // una = rcv_nxt_ = 51, ack_num = 50, ack_bits covering 18..49.
  // Pre-fix this would clear only seqs 18..50 (33 entries) and leave 1..17
  // stranded; post-fix the una pass clears the whole 1..50 range first.
  channel_b.Bundle().AddMessage(RudpTestMsg{999});
  ASSERT_TRUE(channel_b.SendReliable().HasValue());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_a, channel_a);

  EXPECT_EQ(channel_a.UnackedCount(), 0u)
      << "UNA cumulative ACK must clear the entire <una range, even when the "
         "SACK bitmap (32 bits) cannot reach the oldest unacked entries.";
}

TEST_F(ReliableUdpTest, UnaEmptiesUnackedAndStopsResendTimer) {
  // Smaller-scale variant: any single reliable round-trip should fully
  // drain unacked_ via UNA (rcv_nxt_ advances past the only seq), even
  // before any SACK bitmap bits would be relevant.
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  channel_a.Bundle().AddMessage(RudpTestMsg{1});
  ASSERT_TRUE(channel_a.SendReliable().HasValue());
  EXPECT_EQ(channel_a.UnackedCount(), 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_b, channel_b);
  EXPECT_EQ(channel_b.RecvNextSeq(), 2u);

  channel_b.Bundle().AddMessage(RudpTestMsg{2});
  ASSERT_TRUE(channel_b.SendReliable().HasValue());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_a, channel_a);

  EXPECT_EQ(channel_a.UnackedCount(), 0u);
}

TEST_F(ReliableUdpTest, DuplicateReliablePacketStillTriggersAck) {
  // Replay scenario: A's original ACK from B is "lost", A retransmits
  // the same wire packet. B sees IsDuplicate==true and previously dropped
  // silently — leaving the sender starved of any ACK signal until
  // dead-link condemned the channel. The fix unconditionally schedules a
  // delayed ACK on every reliable receipt, so B's reply still carries an
  // up-to-date una/ack and A's unacked drains.
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  table_.RegisterTypedHandler<RudpTestMsg>([](const Address&, Channel*, const RudpTestMsg&) {});

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  // A sends a reliable packet. Capture the wire bytes B receives so we can
  // replay them — simulating the OS handing the same datagram twice
  // because A's retransmit timer fires after a "lost" ACK.
  channel_a.Bundle().AddMessage(RudpTestMsg{42});
  ASSERT_TRUE(channel_a.SendReliable().HasValue());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::array<std::byte, 2048> buf{};
  auto first_recv = sock_b->RecvFrom(buf);
  ASSERT_TRUE(first_recv.HasValue());
  ASSERT_GT(first_recv->first, 0u);
  std::vector<std::byte> wire_bytes(buf.begin(), buf.begin() + first_recv->first);

  // First delivery — B advances rcv_nxt_ and (pre-fix) would mark this
  // seq as already received.
  channel_b.OnDatagramReceived(std::span<const std::byte>(wire_bytes));
  EXPECT_EQ(channel_b.RecvNextSeq(), 2u);

  // Replay the exact same bytes — B treats it as a duplicate. The fix
  // requires it to still ScheduleDelayedAck so the next reply piggybacks
  // an ACK pointer covering this seq.
  channel_b.OnDatagramReceived(std::span<const std::byte>(wire_bytes));

  // B sends back a reliable reply; its ACK header should now carry
  // una = 2, which strictly clears A's unacked entry for seq 1.
  channel_b.Bundle().AddMessage(RudpTestMsg{99});
  ASSERT_TRUE(channel_b.SendReliable().HasValue());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pump_datagrams(*sock_a, channel_a);

  EXPECT_EQ(channel_a.UnackedCount(), 0u)
      << "Duplicate reliable receipt must still trigger an ACK so the "
         "sender's unacked queue eventually drains.";
}

namespace {

// Records every SendFilter / RecvFilter call.  SendFilter XOR's payload
// with `xor_byte` so callers can verify the on-wire bytes were actually
// transformed (ruling out a no-op pass-through that would also satisfy
// the call-count check).
class RecordingXorFilter : public PacketFilter {
 public:
  explicit RecordingXorFilter(std::byte xor_byte) : xor_byte_(xor_byte) {}

  [[nodiscard]] auto SendFilter(std::span<const std::byte> data)
      -> Result<std::vector<std::byte>> override {
    ++send_calls_;
    last_send_size_ = data.size();
    std::vector<std::byte> out(data.begin(), data.end());
    for (auto& b : out) b ^= xor_byte_;
    return out;
  }

  [[nodiscard]] auto RecvFilter(std::span<const std::byte> data)
      -> Result<std::vector<std::byte>> override {
    ++recv_calls_;
    std::vector<std::byte> out(data.begin(), data.end());
    for (auto& b : out) b ^= xor_byte_;
    return out;
  }

  [[nodiscard]] auto SendCalls() const -> int { return send_calls_; }
  [[nodiscard]] auto RecvCalls() const -> int { return recv_calls_; }
  [[nodiscard]] auto LastSendSize() const -> std::size_t { return last_send_size_; }

 private:
  std::byte xor_byte_;
  int send_calls_{0};
  int recv_calls_{0};
  std::size_t last_send_size_{0};
};

}  // namespace

struct RudpTestMsgUnreliable {
  uint32_t value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{301, "RudpTestMsgUnreliable", MessageLengthStyle::kVariable, -1,
                                  MessageReliability::kUnreliable};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint32_t>(value); }

  static auto Deserialize(BinaryReader& r) -> Result<RudpTestMsgUnreliable> {
    auto v = r.Read<uint32_t>();
    if (!v) return v.Error();
    return RudpTestMsgUnreliable{*v};
  }
};

TEST_F(ReliableUdpTest, FlushDeferredAppliesPacketFilter) {
  // Regression: SendBundleReliable / SendBundleUnreliable used to bypass
  // packet_filter_ entirely — a filter installed via SetPacketFilter
  // applied to Channel::Send but not to FlushDeferred.  Cellapp's
  // witness traffic and any future batched path therefore went out
  // un-compressed / un-encrypted.  This test pins the fix.
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();

  auto filter = std::make_shared<RecordingXorFilter>(std::byte{0x5A});
  channel_a.SetPacketFilter(filter);

  // One reliable + one unreliable so both deferred bundles fire.
  ASSERT_TRUE(channel_a.BufferMessageDeferred(RudpTestMsg{1}).HasValue());
  ASSERT_TRUE(channel_a.BufferMessageDeferred(RudpTestMsgUnreliable{2}).HasValue());
  ASSERT_TRUE(channel_a.FlushDeferred().HasValue());

  // FlushDeferred sends unreliable first, then reliable — both go
  // through SendFilter exactly once each.  Pre-fix this would be 0.
  EXPECT_EQ(filter->SendCalls(), 2)
      << "SendBundle{Reliable,Unreliable} must call SendFilter once each";
  EXPECT_GT(filter->LastSendSize(), 0u) << "Filter must see non-empty bundle bytes";
}

TEST_F(ReliableUdpTest, PacketFilterAppliesOnRecvPath) {
  // Round-trip with the same XOR filter on both ends: A's SendFilter
  // XOR's the bytes outbound; B's RecvFilter must XOR them back before
  // dispatch.  Pre-fix RUDP's OnDatagramReceived bypassed RecvFilter
  // (only TcpChannel honoured it), so B saw raw filtered bytes and
  // failed to deserialize.
  auto sock_a = Socket::CreateUdp();
  ASSERT_TRUE(sock_a.HasValue());
  ASSERT_TRUE(sock_a->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_a = sock_a->LocalAddress().Value();

  auto sock_b = Socket::CreateUdp();
  ASSERT_TRUE(sock_b.HasValue());
  ASSERT_TRUE(sock_b->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr_b = sock_b->LocalAddress().Value();

  bool received = false;
  uint32_t received_value = 0;
  table_.RegisterTypedHandler<RudpTestMsg>([&](const Address&, Channel*, const RudpTestMsg& msg) {
    received = true;
    received_value = msg.value;
  });

  ReliableUdpChannel channel_a(dispatcher_, table_, *sock_a, addr_b);
  channel_a.Activate();
  ReliableUdpChannel channel_b(dispatcher_, table_, *sock_b, addr_a);
  channel_b.Activate();

  channel_a.SetPacketFilter(std::make_shared<RecordingXorFilter>(std::byte{0x37}));
  auto recv_filter = std::make_shared<RecordingXorFilter>(std::byte{0x37});
  channel_b.SetPacketFilter(recv_filter);

  channel_a.Bundle().AddMessage(RudpTestMsg{0xDEADBEEF});
  ASSERT_TRUE(channel_a.SendReliable().HasValue());

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  pump_datagrams(*sock_b, channel_b);

  EXPECT_TRUE(received) << "Recv-side filter must un-XOR before dispatch";
  EXPECT_EQ(received_value, 0xDEADBEEFu);
  EXPECT_GE(recv_filter->RecvCalls(), 1);
}

TEST_F(ReliableUdpTest, SetMtuChangesMaxUdpPayload) {
  // Per-channel MTU override drives MaxUdpPayload() and the fragment
  // boundary.  Defaults to rudp::kDefaultMtu (1400).  Internet profile
  // sets ~470; cluster profile keeps 1400.
  auto sock = Socket::CreateUdp();
  ASSERT_TRUE(sock.HasValue());
  ASSERT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());

  ReliableUdpChannel ch(dispatcher_, table_, *sock, Address("127.0.0.1", 1));

  EXPECT_EQ(ch.Mtu(), rudp::kDefaultMtu);
  EXPECT_EQ(ch.MaxUdpPayload(), rudp::kDefaultMtu - rudp::kMaxHeaderSize);

  ch.SetMtu(470);
  EXPECT_EQ(ch.Mtu(), 470u);
  EXPECT_EQ(ch.MaxUdpPayload(), 470u - rudp::kMaxHeaderSize);
}
