// Integration test: TCP bidirectional message round-trip between two
// NetworkInterface instances running in the same event-loop thread.
//
// Scenario:
//   - "Server" NI binds a TCP listener.
//   - "Client" NI connects to it.
//   - Client sends EchoRequest; server echoes back EchoReply.
//   - Test verifies payload fidelity and bytes_sent / bytes_received statistics.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/message.h"
#include "network/network_interface.h"
#include "network/tcp_channel.h"

using namespace atlas;

// ============================================================================
// Message definitions
// ============================================================================

struct EchoRequest {
  uint32_t id{0};
  std::string text;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{400, "EchoRequest", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(id);
    w.WriteString(text);
  }

  static auto Deserialize(BinaryReader& r) -> Result<EchoRequest> {
    EchoRequest msg;
    auto i = r.Read<uint32_t>();
    if (!i) return i.Error();
    msg.id = *i;
    auto t = r.ReadString();
    if (!t) return t.Error();
    msg.text = std::move(*t);
    return msg;
  }
};

struct EchoReply {
  uint32_t id{0};
  std::string text;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{401, "EchoReply", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(id);
    w.WriteString(text);
  }

  static auto Deserialize(BinaryReader& r) -> Result<EchoReply> {
    EchoReply msg;
    auto i = r.Read<uint32_t>();
    if (!i) return i.Error();
    msg.id = *i;
    auto t = r.ReadString();
    if (!t) return t.Error();
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
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
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

class TcpEchoRoundtripTest : public ::testing::Test {
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
// Test: single request → reply
// ============================================================================

TEST_F(TcpEchoRoundtripTest, SingleRequestReply) {
  // ---- Server setup -------------------------------------------------------
  ASSERT_TRUE(server_ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());
  auto server_addr = server_ni_.TcpAddress();

  // Server echoes every EchoRequest back as an EchoReply on the same channel.
  server_ni_.InterfaceTable().RegisterTypedHandler<EchoRequest>(
      [&](const Address&, Channel* ch, const EchoRequest& req) {
        EchoReply reply{req.id, req.text};
        (void)ch->SendMessage(reply);
      });

  // ---- Client setup -------------------------------------------------------
  std::atomic<bool> reply_received{false};
  std::atomic<uint32_t> reply_id{0};
  std::string reply_text;

  client_ni_.InterfaceTable().RegisterTypedHandler<EchoReply>(
      [&](const Address&, Channel*, const EchoReply& rep) {
        reply_id.store(rep.id, std::memory_order_relaxed);
        reply_text = rep.text;
        reply_received.store(true, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectTcp(server_addr);
  ASSERT_TRUE(conn.HasValue()) << conn.Error().Message();

  // Wait until the server has accepted the connection.
  ASSERT_TRUE(
      poll_both_until(server_disp_, client_disp_, [&] { return server_ni_.ChannelCount() >= 1u; }));

  // Client sends the request.
  auto* channel = *conn;
  auto send_result = channel->SendMessage(EchoRequest{7, "hello"});
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();
  client_ni_.FlushDirtySendChannels();

  // Wait for the round-trip to complete.
  ASSERT_TRUE(poll_both_until(server_disp_, client_disp_, [&] {
    return reply_received.load(std::memory_order_acquire);
  })) << "EchoReply was never received within the timeout";

  EXPECT_EQ(reply_id.load(), 7u);
  EXPECT_EQ(reply_text, "hello");
}

TEST_F(TcpEchoRoundtripTest, LargeFrameRoundtripTriggersDynamicBufferGrowth) {
  ASSERT_TRUE(server_ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());
  auto server_addr = server_ni_.TcpAddress();

  server_ni_.InterfaceTable().RegisterTypedHandler<EchoRequest>(
      [&](const Address&, Channel* ch, const EchoRequest& req) {
        (void)ch->SendMessage(EchoReply{req.id, req.text});
      });

  std::atomic<bool> reply_received{false};
  std::atomic<uint32_t> reply_id{0};
  std::string reply_text;

  client_ni_.InterfaceTable().RegisterTypedHandler<EchoReply>(
      [&](const Address&, Channel*, const EchoReply& rep) {
        reply_id.store(rep.id, std::memory_order_relaxed);
        reply_text = rep.text;
        reply_received.store(true, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectTcp(server_addr);
  ASSERT_TRUE(conn.HasValue()) << conn.Error().Message();
  ASSERT_TRUE(
      poll_both_until(server_disp_, client_disp_, [&] { return server_ni_.ChannelCount() >= 1u; }));

  std::string payload(48 * 1024, 'x');
  auto send_result = (*conn)->SendMessage(EchoRequest{99, payload});
  ASSERT_TRUE(send_result.HasValue()) << send_result.Error().Message();
  client_ni_.FlushDirtySendChannels();

  ASSERT_TRUE(poll_both_until(
      server_disp_, client_disp_, [&] { return reply_received.load(std::memory_order_acquire); },
      std::chrono::milliseconds(2000)))
      << "Large EchoReply was never received within the timeout";

  EXPECT_EQ(reply_id.load(), 99u);
  EXPECT_EQ(reply_text, payload);
}

// ============================================================================
// Test: multiple sequential requests preserve order and identity
// ============================================================================

TEST_F(TcpEchoRoundtripTest, MultipleRequestsInOrder) {
  ASSERT_TRUE(server_ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  server_ni_.InterfaceTable().RegisterTypedHandler<EchoRequest>(
      [&](const Address&, Channel* ch, const EchoRequest& req) {
        (void)ch->SendMessage(EchoReply{req.id, req.text});
      });

  std::vector<uint32_t> received_ids;
  std::atomic<int> reply_count{0};
  constexpr int kRequests = 5;

  client_ni_.InterfaceTable().RegisterTypedHandler<EchoReply>(
      [&](const Address&, Channel*, const EchoReply& rep) {
        received_ids.push_back(rep.id);
        reply_count.fetch_add(1, std::memory_order_release);
      });

  auto conn = client_ni_.ConnectTcp(server_ni_.TcpAddress());
  ASSERT_TRUE(conn.HasValue());
  ASSERT_TRUE(
      poll_both_until(server_disp_, client_disp_, [&] { return server_ni_.ChannelCount() >= 1u; }));

  auto* ch = *conn;
  for (uint32_t i = 0; i < kRequests; ++i) {
    (void)ch->SendMessage(EchoRequest{i, std::to_string(i)});
    client_ni_.FlushDirtySendChannels();
  }

  ASSERT_TRUE(poll_both_until(
      server_disp_, client_disp_,
      [&] { return reply_count.load(std::memory_order_acquire) >= kRequests; },
      std::chrono::milliseconds(2000)));

  ASSERT_EQ(static_cast<int>(received_ids.size()), kRequests);
  for (int i = 0; i < kRequests; ++i) {
    EXPECT_EQ(received_ids[i], static_cast<uint32_t>(i));
  }
}

// ============================================================================
// Test: bytes_sent increases after sending
// ============================================================================

TEST_F(TcpEchoRoundtripTest, BytesSentTracked) {
  ASSERT_TRUE(server_ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  // Server doesn't need to reply; we only care about the sender's stats.
  server_ni_.InterfaceTable().RegisterTypedHandler<EchoRequest>(
      [](const Address&, Channel*, const EchoRequest&) {});

  auto conn = client_ni_.ConnectTcp(server_ni_.TcpAddress());
  ASSERT_TRUE(conn.HasValue());
  ASSERT_TRUE(
      poll_both_until(server_disp_, client_disp_, [&] { return server_ni_.ChannelCount() >= 1u; }));

  auto* ch = *conn;
  EXPECT_EQ(ch->BytesSent(), 0u);

  (void)ch->SendMessage(EchoRequest{1, "payload"});
  client_ni_.FlushDirtySendChannels();
  EXPECT_GT(ch->BytesSent(), 0u);
}

// ============================================================================
// Test: prepare_for_shutdown condemns all server-side channels immediately,
// reducing channel_count() to zero.
// ============================================================================

TEST_F(TcpEchoRoundtripTest, ServerShutdownCondemnedServerChannels) {
  ASSERT_TRUE(server_ni_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());

  auto conn = client_ni_.ConnectTcp(server_ni_.TcpAddress());
  ASSERT_TRUE(conn.HasValue());

  // Wait for the server to accept the connection.
  ASSERT_TRUE(
      poll_both_until(server_disp_, client_disp_, [&] { return server_ni_.ChannelCount() >= 1u; }));
  EXPECT_EQ(server_ni_.ChannelCount(), 1u);

  // Shut down the server: all accepted channels are moved to the condemned
  // list and removed from the active map immediately.
  server_ni_.PrepareForShutdown();

  // channel_count() reflects only the live (active) map — it should be zero
  // immediately after prepare_for_shutdown().
  EXPECT_EQ(server_ni_.ChannelCount(), 0u);
}
