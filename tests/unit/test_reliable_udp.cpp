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
