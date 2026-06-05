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

TEST(MeshGossip, RegistryMsgRoundTrip) {
  MeshRegistryMsg sent;
  sent.owner = Address("10.0.0.7", 20020);
  ProcessInfo a;
  a.process_type = ProcessType::kCellApp;
  a.name = "cellapp-1";
  a.internal_addr = Address("10.0.0.7", 30001);
  a.external_addr = Address("10.0.0.7", 40001);
  a.pid = 1234;
  a.load = 0.25f;
  sent.processes.push_back(a);
  ProcessInfo b;
  b.process_type = ProcessType::kBaseApp;
  b.name = "baseapp-1";
  b.pid = 5678;
  sent.processes.push_back(b);

  BinaryWriter w;
  sent.Serialize(w);
  BinaryReader r(w.Data());
  auto got = MeshRegistryMsg::Deserialize(r);
  ASSERT_TRUE(got.HasValue()) << got.Error().Message();
  EXPECT_EQ(got->owner.Port(), 20020);
  ASSERT_EQ(got->processes.size(), 2u);
  EXPECT_EQ(got->processes[0].process_type, ProcessType::kCellApp);
  EXPECT_EQ(got->processes[0].name, "cellapp-1");
  EXPECT_EQ(got->processes[0].pid, 1234u);
  EXPECT_FLOAT_EQ(got->processes[0].load, 0.25f);
  EXPECT_EQ(got->processes[1].name, "baseapp-1");
  EXPECT_EQ(r.Remaining(), 0u);
}

TEST(MeshGossip, RegistryMsgRejectsWrongType) {
  MeshHello hello;
  hello.machined_addr = Address("127.0.0.1", 20018);
  BinaryWriter w;
  hello.Serialize(w);
  BinaryReader r(w.Data());
  auto got = MeshRegistryMsg::Deserialize(r);
  EXPECT_FALSE(got.HasValue());
}

TEST(MeshGossip, ProcessDeathRoundTrip) {
  MeshProcessDeath sent;
  sent.dead_machined = Address("10.0.0.9", 20020);
  BinaryWriter w;
  sent.Serialize(w);
  BinaryReader r(w.Data());
  auto got = MeshProcessDeath::Deserialize(r);
  ASSERT_TRUE(got.HasValue()) << got.Error().Message();
  EXPECT_EQ(got->dead_machined.Ip(), sent.dead_machined.Ip());
  EXPECT_EQ(got->dead_machined.Port(), 20020);
}

TEST(MeshGossip, PeekTypeDistinguishesMessages) {
  BinaryWriter hw;
  MeshHello{}.Serialize(hw);
  BinaryReader hr(hw.Data());
  auto ht = PeekMeshType(hr);
  ASSERT_TRUE(ht.HasValue());
  EXPECT_EQ(*ht, MeshMessageType::kHello);

  BinaryWriter rw;
  MeshRegistryMsg{}.Serialize(rw);
  BinaryReader rr(rw.Data());
  auto rt = PeekMeshType(rr);
  ASSERT_TRUE(rt.HasValue());
  EXPECT_EQ(*rt, MeshMessageType::kRegistry);

  BinaryWriter dw;
  MeshProcessDeath{}.Serialize(dw);
  BinaryReader dr(dw.Data());
  auto dt = PeekMeshType(dr);
  ASSERT_TRUE(dt.HasValue());
  EXPECT_EQ(*dt, MeshMessageType::kProcessDeath);
}

}  // namespace
}  // namespace atlas::machined
