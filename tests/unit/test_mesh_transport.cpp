#include "server/mesh_transport.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "foundation/clock.h"
#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/mesh_gossip.h"
#include "serialization/binary_stream.h"

using namespace atlas;

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

static auto Loopback(uint16_t port) -> Address { return Address("127.0.0.1", port); }

static auto Encode(const machined::MeshHello& hello) -> std::vector<std::byte> {
  BinaryWriter w;
  hello.Serialize(w);
  return w.Detach();
}

static auto DecodeHello(std::span<const std::byte> payload) -> std::optional<machined::MeshHello> {
  BinaryReader r(payload);
  auto h = machined::MeshHello::Deserialize(r);
  if (!h) return std::nullopt;
  return *h;
}

TEST(MeshTransport, DeliversDirectedDatagram) {
  EventDispatcher disp{"mesh_test"};
  disp.SetMaxPollWait(Milliseconds(1));
  MeshTransport sender(disp);
  MeshTransport receiver(disp);
  ASSERT_TRUE(sender.Open(Loopback(0), Loopback(0)).HasValue());
  ASSERT_TRUE(receiver.Open(Loopback(0), Loopback(0)).HasValue());

  std::optional<machined::MeshHello> got;
  Address got_src;
  receiver.SetDatagramCallback([&](const Address& src, std::span<const std::byte> payload) {
    got = DecodeHello(payload);
    got_src = src;
  });

  machined::MeshHello hello;
  hello.machined_addr = sender.LocalAddress();
  hello.incarnation = 0xC0FFEEULL;
  ASSERT_TRUE(sender.SendTo(receiver.LocalAddress(), Encode(hello)).HasValue());

  ASSERT_TRUE(poll_until(disp, [&] { return got.has_value(); }));
  EXPECT_EQ(got->incarnation, 0xC0FFEEULL);
  EXPECT_EQ(got->machined_addr.Port(), sender.LocalAddress().Port());
  EXPECT_EQ(got_src.Port(), sender.LocalAddress().Port());
}

TEST(MeshTransport, BroadcastSendsToConfiguredAddress) {
  EventDispatcher disp{"mesh_test"};
  disp.SetMaxPollWait(Milliseconds(1));
  MeshTransport receiver(disp);
  ASSERT_TRUE(receiver.Open(Loopback(0), Loopback(0)).HasValue());

  // Point the sender's broadcast address at the receiver so Broadcast's delivery
  // is deterministic (real 255.255.255.255 routing is exercised live).
  MeshTransport sender(disp);
  ASSERT_TRUE(sender.Open(Loopback(0), receiver.LocalAddress()).HasValue());

  std::optional<machined::MeshHello> got;
  receiver.SetDatagramCallback(
      [&](const Address&, std::span<const std::byte> payload) { got = DecodeHello(payload); });

  machined::MeshHello hello;
  hello.machined_addr = sender.LocalAddress();
  hello.incarnation = 7;
  ASSERT_TRUE(sender.Broadcast(Encode(hello)).HasValue());

  ASSERT_TRUE(poll_until(disp, [&] { return got.has_value(); }));
  EXPECT_EQ(got->incarnation, 7u);
}

TEST(MeshTransport, SendBeforeOpenFails) {
  EventDispatcher disp{"mesh_test"};
  MeshTransport t(disp);
  EXPECT_FALSE(t.IsOpen());
  machined::MeshHello hello;
  EXPECT_FALSE(t.SendTo(Loopback(20018), Encode(hello)).HasValue());
}

TEST(MeshTransport, CloseStopsDelivery) {
  EventDispatcher disp{"mesh_test"};
  disp.SetMaxPollWait(Milliseconds(1));
  MeshTransport sender(disp);
  MeshTransport receiver(disp);
  ASSERT_TRUE(sender.Open(Loopback(0), Loopback(0)).HasValue());
  ASSERT_TRUE(receiver.Open(Loopback(0), Loopback(0)).HasValue());
  const Address receiver_addr = receiver.LocalAddress();

  int hits = 0;
  receiver.SetDatagramCallback([&](const Address&, std::span<const std::byte>) { ++hits; });
  receiver.Close();
  EXPECT_FALSE(receiver.IsOpen());

  machined::MeshHello hello;
  hello.machined_addr = sender.LocalAddress();
  ASSERT_TRUE(sender.SendTo(receiver_addr, Encode(hello)).HasValue());

  // No reader is registered after Close, so the callback must never fire.
  poll_until(disp, [&] { return hits > 0; }, std::chrono::milliseconds(50));
  EXPECT_EQ(hits, 0);
}
