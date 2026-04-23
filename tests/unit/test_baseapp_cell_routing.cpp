// BaseApp multi-CellApp routing. Exercises the pure ResolveCellChannelByAddr
// helper that backs OnClientCellRpc's per-entity routing. The helper takes
// just a map and an Address, so tests drive it with fake Channel* values
// (compared for identity only, never dereferenced).

#include <cstdint>
#include <unordered_map>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "network/address.h"
#include "network/channel.h"

namespace atlas {
namespace {

auto FakeChannel(uintptr_t tag) -> Channel* {
  return reinterpret_cast<Channel*>(tag);
}

TEST(BaseAppCellRouting, EmptyMap_ReturnsNullptr) {
  std::unordered_map<Address, Channel*> channels;
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), nullptr);
}

TEST(BaseAppCellRouting, ZeroPortCellAddr_ReturnsNullptr) {
  // Default-constructed Address (what BaseEntity::cell_addr_ is before
  // OnCellEntityCreated fires) must map to "cannot route" even if the
  // map happens to contain an Address that hashes the same way.
  std::unordered_map<Address, Channel*> channels;
  channels[Address(0, 0)] = FakeChannel(0xAA);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0, 0)), nullptr);
}

TEST(BaseAppCellRouting, KnownAddr_ReturnsMatchingChannel) {
  auto* c1 = FakeChannel(0x11);
  std::unordered_map<Address, Channel*> channels;
  channels[Address(0x7F000001u, 30001)] = c1;
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), c1);
}

TEST(BaseAppCellRouting, UnknownAddr_ReturnsNullptrDespiteOtherPeers) {
  auto* c1 = FakeChannel(0x11);
  std::unordered_map<Address, Channel*> channels;
  channels[Address(0x7F000001u, 30001)] = c1;
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30002)), nullptr);
}

TEST(BaseAppCellRouting, MultiPeerMap_RoutesEachEntityToItsOwnChannel) {
  // The core multi-CellApp scenario: two entities on different Cells
  // must route to different channels. OnClientCellRpc delegates this
  // choice to ResolveCellChannelByAddr via the per-entity cell_addr.
  auto* c_peer_a = FakeChannel(0x11);
  auto* c_peer_b = FakeChannel(0x22);
  std::unordered_map<Address, Channel*> channels;
  channels[Address(0x7F000001u, 30001)] = c_peer_a;
  channels[Address(0x7F000001u, 30002)] = c_peer_b;

  // Entity on peer A.
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), c_peer_a);
  // Entity on peer B.
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30002)), c_peer_b);
}

TEST(BaseAppCellRouting, PeerRemoval_StaleAddrNoLongerResolves) {
  // Simulates the CellApp Death callback erasing its entry.
  auto* c1 = FakeChannel(0x11);
  std::unordered_map<Address, Channel*> channels;
  channels[Address(0x7F000001u, 30001)] = c1;
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), c1);

  channels.erase(Address(0x7F000001u, 30001));
  EXPECT_EQ(ResolveCellChannelByAddr(channels, Address(0x7F000001u, 30001)), nullptr);
}

TEST(BaseAppCellRouting, OffloadLikeHandoff_RoutesTargetsToTheirNewChannels) {
  // Models the aftermath of an Offload: an entity previously on peer A
  // has its cell_addr updated to peer B (via OnCurrentCell). The
  // routing helper itself doesn't see entities — BaseApp's per-entity
  // wrapper does — but this confirms that once the cell_addr input
  // flips, the helper's resolution flips with it.
  auto* c_peer_a = FakeChannel(0x11);
  auto* c_peer_b = FakeChannel(0x22);
  std::unordered_map<Address, Channel*> channels;
  channels[Address(0x7F000001u, 30001)] = c_peer_a;
  channels[Address(0x7F000001u, 30002)] = c_peer_b;

  // Before Offload: entity lives at peer A.
  Address entity_cell_addr(0x7F000001u, 30001);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, entity_cell_addr), c_peer_a);

  // OnCurrentCell lands — entity now lives at peer B.
  entity_cell_addr = Address(0x7F000001u, 30002);
  EXPECT_EQ(ResolveCellChannelByAddr(channels, entity_cell_addr), c_peer_b);
}

}  // namespace
}  // namespace atlas
