// BaseApp multi-CellApp routing. Exercises the pure ResolveCellChannelByAddr
// helper that backs OnClientCellRpc's per-entity routing.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "foundation/intrusive_ptr.h"
#include "network/address.h"
#include "network/channel.h"
#include "test_null_channel.h"

namespace atlas {
namespace {

auto FakeChannel() -> IntrusivePtr<Channel> {
  return IntrusivePtr<Channel>{test_support::FakeChannel()};
}

TEST(BaseAppCellRouting, EmptyMap_ReturnsNullptr) {
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), nullptr);
}

TEST(BaseAppCellRouting, ZeroPortCellAddr_ReturnsNullptr) {
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  channels[Address(0, 0)] = FakeChannel();
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0, 0)), nullptr);
}

TEST(BaseAppCellRouting, KnownAddr_ReturnsMatchingChannel) {
  auto c1 = FakeChannel();
  auto* c1_raw = c1.get();
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  channels[Address(0x7F000001u, 30001)] = std::move(c1);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), c1_raw);
}

TEST(BaseAppCellRouting, UnknownAddr_ReturnsNullptrDespiteOtherPeers) {
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  channels[Address(0x7F000001u, 30001)] = FakeChannel();
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30002)), nullptr);
}

TEST(BaseAppCellRouting, MultiPeerMap_RoutesEachEntityToItsOwnChannel) {
  auto a = FakeChannel();
  auto b = FakeChannel();
  auto* a_raw = a.get();
  auto* b_raw = b.get();
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  channels[Address(0x7F000001u, 30001)] = std::move(a);
  channels[Address(0x7F000001u, 30002)] = std::move(b);

  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), a_raw);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30002)), b_raw);
}

TEST(BaseAppCellRouting, PeerRemoval_StaleAddrNoLongerResolves) {
  auto c1 = FakeChannel();
  auto* c1_raw = c1.get();
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  channels[Address(0x7F000001u, 30001)] = std::move(c1);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), c1_raw);

  channels.erase(Address(0x7F000001u, 30001));
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), nullptr);
}

TEST(BaseAppCellRouting, OffloadLikeHandoff_RoutesTargetsToTheirNewChannels) {
  auto a = FakeChannel();
  auto b = FakeChannel();
  auto* a_raw = a.get();
  auto* b_raw = b.get();
  std::unordered_map<Address, IntrusivePtr<Channel>> channels;
  channels[Address(0x7F000001u, 30001)] = std::move(a);
  channels[Address(0x7F000001u, 30002)] = std::move(b);

  Address entity_cell_addr(0x7F000001u, 30001);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, entity_cell_addr), a_raw);

  entity_cell_addr = Address(0x7F000001u, 30002);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, entity_cell_addr), b_raw);
}

}  // namespace
}  // namespace atlas
