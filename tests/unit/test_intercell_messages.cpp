// Inter-CellApp message roundtrip tests — Phase 11 PR-1.
//
// Locks down the Serialize/Deserialize contract for every Real/Ghost +
// Offload message so wire format changes surface as test failures rather
// than silent cross-CellApp corruption.

#include <cstddef>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "cellapp/intercell_messages.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::cellapp;

namespace {

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

auto MakeBlob(std::initializer_list<uint8_t> xs) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(xs.size());
  for (auto v : xs) out.push_back(std::byte{v});
  return out;
}

}  // namespace

// ─── CreateGhost ──────────────────────────────────────────────────────────────

TEST(IntercellMessages, CreateGhost_RoundTrip) {
  CreateGhost msg;
  msg.real_entity_id = 0x01020304;
  msg.type_id = 42;
  msg.space_id = 7;
  msg.position = {1.5f, -2.25f, 3.75f};
  msg.direction = {0.f, 1.f, 0.f};
  msg.on_ground = true;
  msg.real_cellapp_addr = Address(0x7F000001u, 30001);
  msg.base_addr = Address(0x0A000002u, 20002);
  msg.base_entity_id = 0x00ABCDEF;
  msg.event_seq = 100;
  msg.volatile_seq = 200;
  msg.other_snapshot = MakeBlob({0xDE, 0xAD, 0xBE, 0xEF, 0x42});

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->real_entity_id, msg.real_entity_id);
  EXPECT_EQ(rt->type_id, msg.type_id);
  EXPECT_EQ(rt->space_id, msg.space_id);
  EXPECT_FLOAT_EQ(rt->position.x, msg.position.x);
  EXPECT_FLOAT_EQ(rt->position.z, msg.position.z);
  EXPECT_FLOAT_EQ(rt->direction.y, 1.f);
  EXPECT_TRUE(rt->on_ground);
  EXPECT_EQ(rt->real_cellapp_addr.Port(), 30001u);
  EXPECT_EQ(rt->base_addr.Ip(), 0x0A000002u);
  EXPECT_EQ(rt->base_entity_id, 0x00ABCDEFu);
  EXPECT_EQ(rt->event_seq, 100u);
  EXPECT_EQ(rt->volatile_seq, 200u);
  ASSERT_EQ(rt->other_snapshot.size(), 5u);
  EXPECT_EQ(rt->other_snapshot[0], std::byte{0xDE});
  EXPECT_EQ(rt->other_snapshot[4], std::byte{0x42});
}

TEST(IntercellMessages, CreateGhost_EmptySnapshot) {
  CreateGhost msg;
  msg.real_entity_id = 1;
  msg.type_id = 1;
  msg.space_id = 1;
  msg.real_cellapp_addr = Address(0, 1);
  msg.base_addr = Address(0, 1);

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->other_snapshot.empty());
}

// ─── DeleteGhost ──────────────────────────────────────────────────────────────

TEST(IntercellMessages, DeleteGhost_RoundTrip) {
  DeleteGhost msg{0x12345678};
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->ghost_entity_id, 0x12345678u);
}

// ─── GhostPositionUpdate ──────────────────────────────────────────────────────

TEST(IntercellMessages, GhostPositionUpdate_RoundTrip) {
  GhostPositionUpdate msg;
  msg.ghost_entity_id = 99;
  msg.position = {10.0f, 20.0f, 30.0f};
  msg.direction = {-1.0f, 0.0f, 0.0f};
  msg.on_ground = false;
  msg.volatile_seq = 0xDEADBEEFCAFE;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->ghost_entity_id, 99u);
  EXPECT_FLOAT_EQ(rt->position.y, 20.0f);
  EXPECT_FLOAT_EQ(rt->direction.x, -1.0f);
  EXPECT_FALSE(rt->on_ground);
  EXPECT_EQ(rt->volatile_seq, 0xDEADBEEFCAFEull);
}

// ─── GhostDelta ───────────────────────────────────────────────────────────────

TEST(IntercellMessages, GhostDelta_RoundTrip) {
  GhostDelta msg;
  msg.ghost_entity_id = 7;
  msg.event_seq = 50;
  msg.other_delta = MakeBlob({0x01, 0x02, 0x03});

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->ghost_entity_id, 7u);
  EXPECT_EQ(rt->event_seq, 50u);
  ASSERT_EQ(rt->other_delta.size(), 3u);
  EXPECT_EQ(rt->other_delta[1], std::byte{0x02});
}

TEST(IntercellMessages, GhostDelta_EmptyDelta) {
  GhostDelta msg;
  msg.ghost_entity_id = 1;
  msg.event_seq = 1;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->other_delta.empty());
}

// ─── GhostSnapshotRefresh ────────────────────────────────────────────────────

TEST(IntercellMessages, GhostSnapshotRefresh_RoundTrip) {
  GhostSnapshotRefresh msg;
  msg.ghost_entity_id = 55;
  msg.event_seq = 9999;
  msg.other_snapshot = MakeBlob({0xAB, 0xCD});

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->ghost_entity_id, 55u);
  EXPECT_EQ(rt->event_seq, 9999u);
  ASSERT_EQ(rt->other_snapshot.size(), 2u);
  EXPECT_EQ(rt->other_snapshot[0], std::byte{0xAB});
}

// ─── GhostSetReal / GhostSetNextReal ─────────────────────────────────────────

TEST(IntercellMessages, GhostSetReal_RoundTrip) {
  GhostSetReal msg;
  msg.ghost_entity_id = 3;
  msg.new_real_addr = Address(0x7F000002u, 31337);

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->ghost_entity_id, 3u);
  EXPECT_EQ(rt->new_real_addr.Ip(), 0x7F000002u);
  EXPECT_EQ(rt->new_real_addr.Port(), 31337u);
}

TEST(IntercellMessages, GhostSetNextReal_RoundTrip) {
  GhostSetNextReal msg;
  msg.ghost_entity_id = 4;
  msg.next_real_addr = Address(0x0A010101u, 40404);

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->ghost_entity_id, 4u);
  EXPECT_EQ(rt->next_real_addr.Port(), 40404u);
}

// ─── OffloadEntity ───────────────────────────────────────────────────────────

TEST(IntercellMessages, OffloadEntity_RoundTrip_Full) {
  OffloadEntity msg;
  msg.real_entity_id = 0xCAFEBABE;
  msg.type_id = 12;
  msg.space_id = 77;
  msg.position = {-5.f, 0.f, 7.f};
  msg.direction = {1.f, 0.f, 0.f};
  msg.on_ground = true;
  msg.base_addr = Address(0x7F000001u, 20000);
  msg.base_entity_id = 0x11223344;
  msg.persistent_blob = MakeBlob({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
  msg.owner_snapshot = MakeBlob({0xA0, 0xA1});
  msg.other_snapshot = MakeBlob({0xB0, 0xB1, 0xB2});
  msg.latest_event_seq = 1234;
  msg.latest_volatile_seq = 5678;
  msg.controller_data = MakeBlob({0xC0});
  msg.existing_haunts = {Address(0x7F000003u, 30003), Address(0x7F000004u, 30004)};

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->real_entity_id, 0xCAFEBABEu);
  EXPECT_EQ(rt->type_id, 12u);
  EXPECT_EQ(rt->space_id, 77u);
  EXPECT_FLOAT_EQ(rt->position.x, -5.f);
  EXPECT_TRUE(rt->on_ground);
  EXPECT_EQ(rt->base_addr.Port(), 20000u);
  EXPECT_EQ(rt->base_entity_id, 0x11223344u);
  EXPECT_EQ(rt->persistent_blob.size(), 8u);
  EXPECT_EQ(rt->owner_snapshot.size(), 2u);
  EXPECT_EQ(rt->other_snapshot.size(), 3u);
  EXPECT_EQ(rt->latest_event_seq, 1234u);
  EXPECT_EQ(rt->latest_volatile_seq, 5678u);
  ASSERT_EQ(rt->controller_data.size(), 1u);
  EXPECT_EQ(rt->controller_data[0], std::byte{0xC0});
  ASSERT_EQ(rt->existing_haunts.size(), 2u);
  EXPECT_EQ(rt->existing_haunts[0].Port(), 30003u);
  EXPECT_EQ(rt->existing_haunts[1].Port(), 30004u);
}

TEST(IntercellMessages, OffloadEntity_RoundTrip_AllBlobsEmpty) {
  // Minimal offload: no persistent blob, no snapshots, no haunts. Exercises
  // the zero-length branches so a later optimisation cannot regress them.
  OffloadEntity msg;
  msg.real_entity_id = 1;
  msg.type_id = 1;
  msg.space_id = 1;
  msg.base_addr = Address(0, 1);
  msg.base_entity_id = 1;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->persistent_blob.empty());
  EXPECT_TRUE(rt->owner_snapshot.empty());
  EXPECT_TRUE(rt->other_snapshot.empty());
  EXPECT_TRUE(rt->controller_data.empty());
  EXPECT_TRUE(rt->existing_haunts.empty());
}

// ─── OffloadEntityAck ────────────────────────────────────────────────────────

TEST(IntercellMessages, OffloadEntityAck_RoundTripSuccess) {
  OffloadEntityAck msg;
  msg.real_entity_id = 100;
  msg.success = true;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->real_entity_id, 100u);
  EXPECT_TRUE(rt->success);
}

TEST(IntercellMessages, OffloadEntityAck_RoundTripFailure) {
  OffloadEntityAck msg;
  msg.real_entity_id = 200;
  msg.success = false;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->real_entity_id, 200u);
  EXPECT_FALSE(rt->success);
}

// ─── Message ID stability ────────────────────────────────────────────────────
//
// If anyone renumbers these in message_ids.h the wire protocol breaks for
// every running CellApp; assert the allocation explicitly.

TEST(IntercellMessages, MessageIdsAreStable) {
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kCreateGhost), 3100u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kDeleteGhost), 3101u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kGhostPositionUpdate), 3102u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kGhostDelta), 3103u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kGhostSetReal), 3104u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kGhostSetNextReal), 3105u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kGhostSnapshotRefresh), 3106u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kOffloadEntity), 3110u);
  EXPECT_EQ(msg_id::Id(msg_id::CellApp::kOffloadEntityAck), 3111u);
}
