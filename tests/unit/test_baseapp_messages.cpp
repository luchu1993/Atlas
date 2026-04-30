#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp_messages.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::baseapp;

template <typename Msg>
auto round_trip(const Msg& msg) -> std::optional<Msg> {
  BinaryWriter w;
  msg.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto result = Msg::Deserialize(r);
  if (!result) return std::nullopt;
  return std::move(*result);
}

TEST(BaseAppMessages, CreateBase) {
  CreateBase msg;
  msg.type_id = 42;
  msg.entity_id = 12345;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->type_id, 42u);
  EXPECT_EQ(rt->entity_id, 12345u);
}

TEST(BaseAppMessages, CreateBaseFromDB) {
  CreateBaseFromDB msg;
  msg.type_id = 7;
  msg.dbid = 999;
  msg.identifier = "player_001";

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->type_id, 7u);
  EXPECT_EQ(rt->dbid, 999);
  EXPECT_EQ(rt->identifier, "player_001");
}

TEST(BaseAppMessages, AcceptClient) {
  AcceptClient msg;
  msg.dest_entity_id = 77;
  msg.session_key.bytes[0] = 0xDE;
  msg.session_key.bytes[31] = 0xAD;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->dest_entity_id, 77u);
  EXPECT_EQ(rt->session_key.bytes[0], 0xDE);
  EXPECT_EQ(rt->session_key.bytes[31], 0xAD);
}

TEST(BaseAppMessages, CellEntityCreated) {
  CellEntityCreated msg;
  msg.entity_id = 100;
  msg.cell_addr = Address(0x7F000001u, 7002);

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 100u);
  EXPECT_EQ(rt->cell_addr.Port(), 7002u);
}

TEST(BaseAppMessages, CellEntityDestroyed) {
  CellEntityDestroyed msg;
  msg.entity_id = 55;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 55u);
}

TEST(BaseAppMessages, CurrentCell) {
  CurrentCell msg;
  msg.entity_id = 10;
  msg.cell_addr = Address(0x0A000001u, 7003);
  msg.epoch = 42;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 10u);
  EXPECT_EQ(rt->cell_addr.Port(), 7003u);
  EXPECT_EQ(rt->epoch, 42u);
}

TEST(BaseAppMessages, CellRpcForward) {
  CellRpcForward msg;
  msg.entity_id = 5;
  msg.rpc_id = 42;
  msg.payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 5u);
  EXPECT_EQ(rt->rpc_id, 42u);
  ASSERT_EQ(rt->payload.size(), 3u);
  EXPECT_EQ(rt->payload[1], std::byte{0xBB});
}

TEST(BaseAppMessages, BroadcastRpcFromCell) {
  BroadcastRpcFromCell msg;
  msg.rpc_id = 88;
  msg.dest_entity_ids = {3u, 4u, 5u};
  msg.payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->rpc_id, 88u);
  ASSERT_EQ(rt->dest_entity_ids.size(), 3u);
  EXPECT_EQ(rt->dest_entity_ids[0], 3u);
  EXPECT_EQ(rt->dest_entity_ids[2], 5u);
  EXPECT_EQ(rt->payload.size(), 3u);
  EXPECT_FALSE(BroadcastRpcFromCell::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, BroadcastRpcFromCellRejectsTooManyDestinations) {
  BinaryWriter w;
  w.WritePackedInt(88);
  w.WritePackedInt(kMaxBroadcastRpcDestinations + 1);

  auto buf = w.Detach();
  BinaryReader r(buf);
  auto result = BroadcastRpcFromCell::Deserialize(r);

  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(BaseAppMessages, ReplicatedDeltaFromCell) {
  ReplicatedDeltaFromCell msg;
  msg.entity_id = 8;
  msg.delta = {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 8u);
  EXPECT_EQ(rt->delta.size(), 4u);
  EXPECT_EQ(rt->delta[3], std::byte{0x40});
}

TEST(BaseAppMessages, ReplicatedReliableDeltaFromCell) {
  ReplicatedReliableDeltaFromCell msg;
  msg.entity_id = 8;
  msg.delta = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 8u);
  EXPECT_EQ(rt->delta.size(), 4u);
  EXPECT_EQ(rt->delta[0], std::byte{0xDE});
  EXPECT_EQ(rt->delta[3], std::byte{0xEF});
}

// The reliable variant must declare Reliable delivery so the channel routes it
// through the retransmitting path; the unreliable variant must declare the
// opposite. Mixing these up would silently defeat the reliability split.
TEST(BaseAppMessages, DeltaReliabilityDescriptors) {
  EXPECT_TRUE(ReplicatedDeltaFromCell::Descriptor().IsUnreliable());
  EXPECT_FALSE(ReplicatedReliableDeltaFromCell::Descriptor().IsUnreliable());
  EXPECT_NE(ReplicatedDeltaFromCell::Descriptor().id,
            ReplicatedReliableDeltaFromCell::Descriptor().id);
}

TEST(BaseAppMessages, BackupCellEntity) {
  // Periodic cell→base opaque-bytes snapshot. The base stores the payload
  // verbatim; this test confirms round-trip byte fidelity so DB writes /
  // reviver never see mutated blobs.
  BackupCellEntity msg;
  msg.entity_id = 0xCAFEBABE;
  msg.cell_backup_data = {std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
                          std::byte{0x89}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 0xCAFEBABEu);
  ASSERT_EQ(rt->cell_backup_data.size(), 5u);
  EXPECT_EQ(rt->cell_backup_data[0], std::byte{0x01});
  EXPECT_EQ(rt->cell_backup_data[4], std::byte{0x89});
}

TEST(BaseAppMessages, BackupCellEntityReliable) {
  // Must ride the reliable channel — DB writes and reviver cannot tolerate
  // a dropped backup snapshot silently leaving the base with stale bytes.
  EXPECT_FALSE(BackupCellEntity::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, ReplicatedBaselineFromCell) {
  // L4 cell→base relay: opaque owner-snapshot blob whose bytes get
  // forwarded verbatim into a 0xF002 ReplicatedBaselineToClient. Round-
  // trip confirms the payload survives Serialize / Deserialize without
  // mutation — a single flipped byte would mis-deserialize on the client
  // and recover _hp to the wrong value.
  ReplicatedBaselineFromCell msg;
  msg.entity_id = 42;
  msg.snapshot = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 42u);
  ASSERT_EQ(rt->snapshot.size(), 4u);
  EXPECT_EQ(rt->snapshot[0], std::byte{0xAA});
  EXPECT_EQ(rt->snapshot[3], std::byte{0xDD});
}

TEST(BaseAppMessages, ReplicatedBaselineFromCellReliable) {
  // Baselines recover state that's already been missed by the unreliable
  // channel — the one job a dropped baseline *cannot* have is being
  // silent. Must ride the reliable channel.
  EXPECT_FALSE(ReplicatedBaselineFromCell::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, CellAppDeathRejectsTooManyRehomes) {
  BinaryWriter w;
  w.Write<uint32_t>(0x7F000001u);
  w.Write<uint16_t>(30001);
  w.WritePackedInt(kMaxCellAppDeathRehomes + 1);

  auto buf = w.Detach();
  BinaryReader r(buf);
  auto result = CellAppDeath::Deserialize(r);

  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(BaseAppMessages, ClientCellRpc) {
  ClientCellRpc msg;
  msg.target_entity_id = 12345;
  msg.rpc_id = 0x00800042;
  msg.payload = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->target_entity_id, 12345u);
  EXPECT_EQ(rt->rpc_id, 0x00800042u);
  ASSERT_EQ(rt->payload.size(), 3u);
  EXPECT_EQ(rt->payload[0], std::byte{0x11});
  EXPECT_EQ(rt->payload[2], std::byte{0x33});
  EXPECT_EQ(ClientCellRpc::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::BaseApp::kClientCellRpc)));
}

TEST(BaseAppMessages, ClientCellRpcEmptyPayload) {
  ClientCellRpc msg;
  msg.target_entity_id = 1;
  msg.rpc_id = 0x007FFFFF;
  // payload intentionally left empty

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->target_entity_id, 1u);
  EXPECT_EQ(rt->rpc_id, 0x007FFFFFu);
  EXPECT_TRUE(rt->payload.empty());
}

// Locks the three-path CellApp→Client delta contract documented in
// delta_forwarder.h. Any future change that relaxes one of these invariants
// (e.g. making ReplicatedReliableDeltaFromCell unreliable, or overlapping
// msg ids) would silently route ordered property deltas through the
// latest-wins forwarder and desync clients. This test exists precisely to
// fail loud in that case.
TEST(BaseAppMessages, ThreePathDeltaContract) {
  // Path #1 — Unreliable volatile latest-wins, via DeltaForwarder → 0xF001.
  EXPECT_TRUE(ReplicatedDeltaFromCell::Descriptor().IsUnreliable())
      << "Path #1 must be Unreliable; latest-wins is incompatible with Reliable transport.";

  // Path #2 — Reliable property delta, bypasses DeltaForwarder, direct → 0xF003.
  EXPECT_FALSE(ReplicatedReliableDeltaFromCell::Descriptor().IsUnreliable())
      << "Path #2 MUST be Reliable; it carries ordered property deltas (event_seq).";

  // Path #3 — Reliable owner/broadcast RPC, dispatched per-entity via rpc_id.
  EXPECT_FALSE(BroadcastRpcFromCell::Descriptor().IsUnreliable())
      << "Path #3 MUST be Reliable; RPC calls cannot tolerate drops.";

  // All three paths use distinct internal message IDs (so dispatch is unambiguous).
  const auto unreliable_id = ReplicatedDeltaFromCell::Descriptor().id;
  const auto reliable_id = ReplicatedReliableDeltaFromCell::Descriptor().id;
  const auto rpc_id = BroadcastRpcFromCell::Descriptor().id;
  EXPECT_NE(unreliable_id, reliable_id);
  EXPECT_NE(unreliable_id, rpc_id);
  EXPECT_NE(reliable_id, rpc_id);
}

TEST(BaseAppMessages, ReplicatedBaselineToClient) {
  ReplicatedBaselineToClient msg;
  msg.entity_id = 42;
  msg.snapshot = {std::byte{0xCA}, std::byte{0xFE}, std::byte{0xBA}, std::byte{0xBE}};

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 42u);
  EXPECT_EQ(rt->snapshot.size(), 4u);
  EXPECT_EQ(rt->snapshot[0], std::byte{0xCA});
  EXPECT_EQ(rt->snapshot[3], std::byte{0xBE});
}

// Baseline is the loss-recovery channel — it MUST be reliable, and it MUST
// use the reserved client-facing ID 0xF002 (neither 0xF001 unreliable delta
// nor 0xF003 reliable delta).
TEST(BaseAppMessages, BaselineDescriptor) {
  const auto& desc = ReplicatedBaselineToClient::Descriptor();
  EXPECT_FALSE(desc.IsUnreliable());
  EXPECT_EQ(desc.id, static_cast<MessageID>(0xF002));
}

// Verify packed_int multi-byte paths: entity_id in [0xFE, 0xFFFF] uses 3-byte
// encoding; entity_id > 0xFFFF uses 5-byte encoding.
TEST(BaseAppMessages, PackedIntBoundaries) {
  // 3-byte entity_id (0xFE tag + uint16 LE)
  {
    ReplicatedDeltaFromCell msg;
    msg.entity_id = 1000;
    msg.delta = {std::byte{0xFF}};
    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->entity_id, 1000u);
    EXPECT_EQ(rt->delta.size(), 1u);
  }
  // 5-byte entity_id (0xFF tag + uint32 LE)
  {
    CellRpcForward msg;
    msg.entity_id = 0x00010001u;
    msg.rpc_id = 500;
    msg.payload = {std::byte{0x01}, std::byte{0x02}};
    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->entity_id, 0x00010001u);
    EXPECT_EQ(rt->rpc_id, 500u);
    EXPECT_EQ(rt->payload.size(), 2u);
  }
  // Large payload size in BroadcastRpcFromCell (crosses 0xFE boundary)
  {
    BroadcastRpcFromCell msg;
    msg.rpc_id = 1;
    msg.dest_entity_ids = {42u};
    msg.payload.assign(300, std::byte{0xAB});
    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    ASSERT_EQ(rt->dest_entity_ids.size(), 1u);
    EXPECT_EQ(rt->dest_entity_ids[0], 42u);
    EXPECT_EQ(rt->payload.size(), 300u);
    EXPECT_EQ(rt->payload[299], std::byte{0xAB});
  }
}

// CellAppDeath carries a variable rehome list (one entry per Space that
// had at least one leaf on the dead CellApp). Exercises empty, single, and
// multi-entry cases to lock in the wire format.
TEST(BaseAppMessages, CellAppDeathRoundTrip_EmptyRehomes) {
  CellAppDeath msg;
  msg.dead_addr = Address(0x7F000001u, 30001);
  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->dead_addr.Ip(), 0x7F000001u);
  EXPECT_EQ(rt->dead_addr.Port(), 30001u);
  EXPECT_TRUE(rt->rehomes.empty());
}

TEST(BaseAppMessages, CellAppDeathRoundTrip_MultipleRehomes) {
  CellAppDeath msg;
  msg.dead_addr = Address(0x0A000001u, 40000);
  msg.rehomes.emplace_back(SpaceID{1}, Address(0x0A000002u, 40001));
  msg.rehomes.emplace_back(SpaceID{42}, Address(0x0A000003u, 40002));
  msg.rehomes.emplace_back(SpaceID{999}, Address(0x0A000004u, 40003));

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->dead_addr.Port(), 40000u);
  ASSERT_EQ(rt->rehomes.size(), 3u);
  EXPECT_EQ(rt->rehomes[0].first, SpaceID{1});
  EXPECT_EQ(rt->rehomes[0].second.Port(), 40001u);
  EXPECT_EQ(rt->rehomes[2].first, SpaceID{999});
  EXPECT_EQ(rt->rehomes[2].second.Ip(), 0x0A000004u);
}
