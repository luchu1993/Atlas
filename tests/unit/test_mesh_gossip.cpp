#include "network/mesh_gossip.h"

#include <cstddef>

#include <gtest/gtest.h>

#include "network/address.h"
#include "serialization/binary_stream.h"

namespace atlas::machined {
namespace {

TEST(MeshGossip, HelloRoundTrip) {
  MeshHello sent;
  sent.machined_addr = Address("10.1.2.3", 20018);
  sent.incarnation = 0xABCDEF0123456789ULL;

  BinaryWriter w;
  sent.Serialize(w);

  BinaryReader r(w.Data());
  auto got = MeshHello::Deserialize(r);
  ASSERT_TRUE(got.HasValue()) << got.Error().Message();
  EXPECT_EQ(got->protocol_version, kMeshProtocolVersion);
  EXPECT_EQ(got->machined_addr.Ip(), sent.machined_addr.Ip());
  EXPECT_EQ(got->machined_addr.Port(), 20018);
  EXPECT_EQ(got->incarnation, sent.incarnation);
  EXPECT_EQ(r.Remaining(), 0u);
}

TEST(MeshGossip, PeekTypeDoesNotConsume) {
  MeshHello sent;
  sent.machined_addr = Address("127.0.0.1", 20018);
  BinaryWriter w;
  sent.Serialize(w);

  BinaryReader r(w.Data());
  auto type = PeekMeshType(r);
  ASSERT_TRUE(type.HasValue());
  EXPECT_EQ(*type, MeshMessageType::kHello);
  // Peeking leaves the tag in place so the body still deserializes.
  auto got = MeshHello::Deserialize(r);
  EXPECT_TRUE(got.HasValue());
}

TEST(MeshGossip, RejectsWrongMessageType) {
  BinaryWriter w;
  w.Write<uint8_t>(0x7F);  // not kHello
  w.Write<uint8_t>(kMeshProtocolVersion);
  w.Write<uint32_t>(0);
  w.Write<uint16_t>(0);
  w.Write<uint64_t>(0);

  BinaryReader r(w.Data());
  auto got = MeshHello::Deserialize(r);
  EXPECT_FALSE(got.HasValue());
}

TEST(MeshGossip, RejectsIncompatibleVersion) {
  BinaryWriter w;
  w.Write<uint8_t>(static_cast<uint8_t>(MeshMessageType::kHello));
  w.Write<uint8_t>(kMeshProtocolVersion + 1);  // unknown version
  w.Write<uint32_t>(0);
  w.Write<uint16_t>(0);
  w.Write<uint64_t>(0);

  BinaryReader r(w.Data());
  auto got = MeshHello::Deserialize(r);
  EXPECT_FALSE(got.HasValue());
}

TEST(MeshGossip, RejectsTruncatedDatagram) {
  MeshHello sent;
  sent.machined_addr = Address("127.0.0.1", 20018);
  sent.incarnation = 42;
  BinaryWriter w;
  sent.Serialize(w);

  // Drop the trailing incarnation bytes.
  auto full = w.Data();
  BinaryReader r(full.subspan(0, full.size() - 4));
  auto got = MeshHello::Deserialize(r);
  EXPECT_FALSE(got.HasValue());
}

}  // namespace
}  // namespace atlas::machined
