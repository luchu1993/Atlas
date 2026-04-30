// CellApp message roundtrip tests.
//
// Serialize/Deserialize contract lock-in for all nine CellApp inbound
// messages. Any byte-format regression here would break wire compat with
// BaseApp→CellApp traffic or break CellApp boot-up.

#include <optional>

#include <gtest/gtest.h>

#include "cellapp_messages.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::cellapp;

template <typename Msg>
auto RoundTrip(const Msg& msg) -> std::optional<Msg> {
  BinaryWriter w;
  msg.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = Msg::Deserialize(r);
  if (!rt) return std::nullopt;
  return std::move(*rt);
}

TEST(CellAppMessages, CreateCellEntityRoundTrip) {
  CreateCellEntity msg;
  msg.entity_id = 42;
  msg.type_id = 7;
  msg.space_id = 100;
  msg.position = {1.5f, 2.5f, 3.5f};
  msg.direction = {0.f, 0.f, 1.f};
  msg.on_ground = true;
  msg.base_addr = Address(0x7F000001u, 12345);
  msg.request_id = 999;
  msg.script_init_data = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 42u);
  EXPECT_EQ(rt->type_id, 7u);
  EXPECT_EQ(rt->space_id, 100u);
  EXPECT_FLOAT_EQ(rt->position.x, 1.5f);
  EXPECT_FLOAT_EQ(rt->position.y, 2.5f);
  EXPECT_FLOAT_EQ(rt->position.z, 3.5f);
  EXPECT_FLOAT_EQ(rt->direction.z, 1.f);
  EXPECT_TRUE(rt->on_ground);
  EXPECT_EQ(rt->base_addr.Ip(), 0x7F000001u);
  EXPECT_EQ(rt->base_addr.Port(), 12345u);
  EXPECT_EQ(rt->request_id, 999u);
  ASSERT_EQ(rt->script_init_data.size(), 3u);
  EXPECT_EQ(rt->script_init_data[0], std::byte{0xAA});
}

TEST(CellAppMessages, CreateCellEntityEmptyScriptDataRoundTrip) {
  CreateCellEntity msg;
  msg.entity_id = 1;
  msg.type_id = 1;
  msg.space_id = 1;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->script_init_data.empty());
}

TEST(CellAppMessages, DestroyCellEntityRoundTrip) {
  DestroyCellEntity msg{123};

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 123u);
}

TEST(CellAppMessages, ClientCellRpcForwardRoundTrip) {
  ClientCellRpcForward msg;
  msg.target_entity_id = 10;
  msg.source_entity_id = 11;  // intentionally different to catch swap bugs
  msg.rpc_id = 0x00800042;
  msg.payload = {std::byte{0x11}, std::byte{0x22}};

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->target_entity_id, 10u);
  EXPECT_EQ(rt->source_entity_id, 11u);
  EXPECT_EQ(rt->rpc_id, 0x00800042u);
  EXPECT_EQ(rt->payload.size(), 2u);
}

TEST(CellAppMessages, InternalCellRpcRoundTrip) {
  InternalCellRpc msg;
  msg.target_entity_id = 77;
  msg.rpc_id = 0x00C00001;
  msg.payload = {std::byte{0xDE}, std::byte{0xAD}};

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->target_entity_id, 77u);
  EXPECT_EQ(rt->rpc_id, 0x00C00001u);
  EXPECT_EQ(rt->payload.size(), 2u);
}

TEST(CellAppMessages, CreateAndDestroySpaceRoundTrip) {
  CreateSpace c{555};
  auto rtc = RoundTrip(c);
  ASSERT_TRUE(rtc.has_value());
  EXPECT_EQ(rtc->space_id, 555u);

  DestroySpace d{555};
  auto rtd = RoundTrip(d);
  ASSERT_TRUE(rtd.has_value());
  EXPECT_EQ(rtd->space_id, 555u);
}

TEST(CellAppMessages, AvatarUpdateRoundTrip) {
  AvatarUpdate msg;
  msg.entity_id = 9;
  msg.position = {100.f, 0.f, -50.f};
  msg.direction = {0.7071f, 0.f, 0.7071f};
  msg.on_ground = false;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 9u);
  EXPECT_FLOAT_EQ(rt->position.x, 100.f);
  EXPECT_FLOAT_EQ(rt->position.z, -50.f);
  EXPECT_FLOAT_EQ(rt->direction.x, 0.7071f);
  EXPECT_FALSE(rt->on_ground);
}

TEST(CellAppMessages, EnableDisableWitnessRoundTrip) {
  EnableWitness e;
  e.entity_id = 17;
  auto rte = RoundTrip(e);
  ASSERT_TRUE(rte.has_value());
  EXPECT_EQ(rte->entity_id, 17u);

  DisableWitness d{17};
  auto rtd = RoundTrip(d);
  ASSERT_TRUE(rtd.has_value());
  EXPECT_EQ(rtd->entity_id, 17u);
}

// Runtime SetAoIRadius. Carries both radius and hysteresis so a single RPC
// reshapes the dual-band AoITrigger.
TEST(CellAppMessages, SetAoIRadiusRoundTrip) {
  SetAoIRadius s;
  s.entity_id = 42;
  s.radius = 120.25f;
  s.hysteresis = 8.5f;

  auto rt = RoundTrip(s);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 42u);
  EXPECT_FLOAT_EQ(rt->radius, 120.25f);
  EXPECT_FLOAT_EQ(rt->hysteresis, 8.5f);

  // ID must be distinct from EnableWitness/DisableWitness — these three
  // share a subsystem but carry independent semantics and must not alias.
  EXPECT_NE(SetAoIRadius::Descriptor().id, EnableWitness::Descriptor().id);
  EXPECT_NE(SetAoIRadius::Descriptor().id, DisableWitness::Descriptor().id);
  EXPECT_EQ(SetAoIRadius::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::CellApp::kSetAoIRadius)));
}

// The two RPC flavours MUST resolve to distinct message IDs — collapsing
// them would let a client-initiated call be mistaken for a trusted
// internal call (and vice versa).
TEST(CellAppMessages, ClientAndInternalRpcHaveDistinctIds) {
  EXPECT_NE(ClientCellRpcForward::Descriptor().id, InternalCellRpc::Descriptor().id);
  EXPECT_EQ(ClientCellRpcForward::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::CellApp::kClientCellRpcForward)));
  EXPECT_EQ(InternalCellRpc::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::CellApp::kInternalCellRpc)));
}

// Range check: every CellApp message id sits in the allocated 3000-3999
// band. The enum's ATLAS_ASSERT_ID_RANGE already catches this at compile
// time; we repeat at runtime so the message struct's Descriptor() path is
// also exercised.
TEST(CellAppMessages, AllMessagesInAllocatedIdRange) {
  auto check = [](MessageID id) {
    EXPECT_GE(static_cast<uint16_t>(id), 3000u);
    EXPECT_LE(static_cast<uint16_t>(id), 3999u);
  };
  check(CreateCellEntity::Descriptor().id);
  check(DestroyCellEntity::Descriptor().id);
  check(ClientCellRpcForward::Descriptor().id);
  check(InternalCellRpc::Descriptor().id);
  check(CreateSpace::Descriptor().id);
  check(DestroySpace::Descriptor().id);
  check(AvatarUpdate::Descriptor().id);
  check(EnableWitness::Descriptor().id);
  check(DisableWitness::Descriptor().id);
  check(SetAoIRadius::Descriptor().id);
}
