#include <array>
#include <limits>
#include <optional>
#include <span>
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

TEST(BaseAppMessages, CellEntityCreateFailed) {
  CellEntityCreateFailed msg;
  msg.entity_id = 100;
  msg.request_id = 77;
  msg.reason = CellEntityCreateFailureReason::kGhostBackupMissing;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 100u);
  EXPECT_EQ(rt->request_id, 77u);
  EXPECT_EQ(rt->reason, CellEntityCreateFailureReason::kGhostBackupMissing);
  EXPECT_EQ(CellEntityCreateFailed::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::BaseApp::kCellEntityCreateFailed)));
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

// Reliable and unreliable delta descriptors must stay distinct so state
// updates cannot silently route through the wrong transport path.
TEST(BaseAppMessages, DeltaReliabilityDescriptors) {
  EXPECT_TRUE(ReplicatedDeltaFromCell::Descriptor().IsUnreliable());
  EXPECT_FALSE(ReplicatedReliableDeltaFromCell::Descriptor().IsUnreliable());
  EXPECT_NE(ReplicatedDeltaFromCell::Descriptor().id,
            ReplicatedReliableDeltaFromCell::Descriptor().id);
}

TEST(BaseAppMessages, BackupCellEntity) {
  // Base stores backup payload bytes verbatim and uses pose only for
  // crash restore placement.
  BackupCellEntity msg;
  msg.entity_id = 0xCAFEBABE;
  msg.cell_backup_data = {std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
                          std::byte{0x89}};
  msg.has_pose = true;
  msg.position = {12.f, 3.f, -4.f};
  msg.direction = {0.f, 0.f, 1.f};
  msg.on_ground = true;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 0xCAFEBABEu);
  ASSERT_EQ(rt->cell_backup_data.size(), 5u);
  EXPECT_EQ(rt->cell_backup_data[0], std::byte{0x01});
  EXPECT_EQ(rt->cell_backup_data[4], std::byte{0x89});
  EXPECT_TRUE(rt->has_pose);
  EXPECT_FLOAT_EQ(rt->position.x, 12.f);
  EXPECT_FLOAT_EQ(rt->position.z, -4.f);
  EXPECT_FLOAT_EQ(rt->direction.z, 1.f);
  EXPECT_TRUE(rt->on_ground);
}

TEST(BaseAppMessages, BackupCellEntityReliable) {
  // Must ride the reliable channel; DB writes and reviver cannot tolerate
  // a dropped backup snapshot silently leaving the base with stale bytes.
  EXPECT_FALSE(BackupCellEntity::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, BackupCellEntityAcceptsLegacyBlobOnlyFrame) {
  BinaryWriter w;
  w.WritePackedInt(123u);
  w.WritePackedInt(2u);
  const std::array<std::byte, 2> bytes{std::byte{0xAA}, std::byte{0xBB}};
  w.WriteBytes(std::span<const std::byte>(bytes));

  auto buf = w.Detach();
  BinaryReader r(buf);
  auto result = BackupCellEntity::Deserialize(r);

  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->entity_id, 123u);
  EXPECT_FALSE(result->has_pose);
  ASSERT_EQ(result->cell_backup_data.size(), 2u);
  EXPECT_EQ(result->cell_backup_data[1], std::byte{0xBB});
}

TEST(BaseAppMessages, ReplicatedBaselineFromCell) {
  // Opaque owner snapshot bytes are forwarded verbatim into
  // ReplicatedBaselineToClient.
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
  // Baselines recover state missed by the unreliable channel and must
  // ride reliable transport.
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

TEST(BaseAppMessages, CellAppDeathRejectsTooManyRehomeCells) {
  BinaryWriter w;
  w.Write<uint32_t>(0x7F000001u);
  w.Write<uint16_t>(30001);
  w.WritePackedInt(0);
  w.WritePackedInt(kMaxCellAppDeathRehomeCells + 1);

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

TEST(BaseAppMessages, ClientMovementInputRoundTrip) {
  ClientMovementInput msg;
  msg.target_entity_id = 77;
  msg.frames.push_back({10, 100, 127, 0, 1024, -3, movement::kInputButtonJump, 16});
  msg.frames.push_back({11, 101, 0, 127, 2048, 2, 0, 17});

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->target_entity_id, 77u);
  ASSERT_EQ(rt->frames.size(), 2u);
  EXPECT_EQ(rt->frames[0].seq, 10u);
  EXPECT_EQ(rt->frames[0].move_x, 127);
  EXPECT_EQ(rt->frames[0].buttons, movement::kInputButtonJump);
  EXPECT_EQ(rt->frames[1].move_z, 127);
  EXPECT_TRUE(ClientMovementInput::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, ClientMovementInputRejectsTooManyFrames) {
  BinaryWriter w;
  w.Write<uint32_t>(77);
  w.Write<uint8_t>(movement::kMaxMovementInputFrames + 1);

  auto buf = w.Detach();
  BinaryReader r(buf);
  auto result = ClientMovementInput::Deserialize(r);

  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(BaseAppMessages, ClientMovementInputRejectsInvalidTarget) {
  ClientMovementInput msg;
  msg.target_entity_id = kInvalidEntityID;
  msg.frames.push_back({10, 100, 127, 0, 1024, -3, 0, movement::kMinInputDtMs});

  auto rt = round_trip(msg);

  EXPECT_FALSE(rt.has_value());
}

TEST(BaseAppMessages, ClientMovementInputRejectsInvalidFrame) {
  ClientMovementInput msg;
  msg.target_entity_id = 77;
  msg.frames.push_back({10, 100, 127, 0, 1024, -3, 0, 0});

  auto rt = round_trip(msg);

  EXPECT_FALSE(rt.has_value());
}

TEST(BaseAppMessages, MovementCorrectionReportRoundTrip) {
  MovementCorrectionReport msg;
  msg.target_entity_id = 77;
  msg.acked_input_seq = 42;
  msg.server_tick = 9001;
  msg.distance_m = movement::kCorrectionTier2DistanceM;
  msg.correction_flags = movement::kCorrectionFlagTier2;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->target_entity_id, 77u);
  EXPECT_EQ(rt->acked_input_seq, 42u);
  EXPECT_EQ(rt->server_tick, 9001u);
  EXPECT_FLOAT_EQ(rt->distance_m, movement::kCorrectionTier2DistanceM);
  EXPECT_EQ(rt->correction_flags, movement::kCorrectionFlagTier2);
  EXPECT_TRUE(MovementCorrectionReport::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementCorrectionReportRejectsInvalidPayload) {
  MovementCorrectionReport msg;
  msg.target_entity_id = 77;
  msg.acked_input_seq = 42;
  msg.server_tick = 9001;
  msg.distance_m = movement::kCorrectionTier2DistanceM;
  msg.correction_flags = movement::kCorrectionFlagTier2;

  auto invalid_target = msg;
  invalid_target.target_entity_id = kInvalidEntityID;
  EXPECT_FALSE(round_trip(invalid_target).has_value());

  auto non_finite = msg;
  non_finite.distance_m = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(round_trip(non_finite).has_value());

  auto unknown_flag = msg;
  unknown_flag.correction_flags = 1u << 7;
  EXPECT_FALSE(round_trip(unknown_flag).has_value());

  auto mismatched_flags = msg;
  mismatched_flags.correction_flags = movement::kCorrectionFlagTier1;
  EXPECT_FALSE(round_trip(mismatched_flags).has_value());
}

// Locks the three-path CellApp->Client delta contract.
// Relaxing these invariants can route ordered deltas through latest-wins.
TEST(BaseAppMessages, ThreePathDeltaContract) {
  // Path #1: unreliable volatile latest-wins via DeltaForwarder.
  EXPECT_TRUE(ReplicatedDeltaFromCell::Descriptor().IsUnreliable())
      << "Path #1 must be Unreliable; latest-wins is incompatible with Reliable transport.";

  // Path #2: reliable property delta bypasses DeltaForwarder.
  EXPECT_FALSE(ReplicatedReliableDeltaFromCell::Descriptor().IsUnreliable())
      << "Path #2 MUST be Reliable; it carries ordered property deltas (event_seq).";

  // Path #3: reliable owner/broadcast RPC dispatched per entity.
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

// Baseline is the reliable loss-recovery channel.
// It must use 0xF002, not the delta wire IDs.
TEST(BaseAppMessages, BaselineDescriptor) {
  const auto& desc = ReplicatedBaselineToClient::Descriptor();
  EXPECT_FALSE(desc.IsUnreliable());
  EXPECT_EQ(desc.id, static_cast<MessageID>(0xF002));
}

TEST(BaseAppMessages, MovementStateAckFromCellRoundTrip) {
  MovementStateAckFromCell msg;
  msg.entity_id = 88;
  msg.acked_input_seq = 120;
  msg.server_tick = 300;
  msg.cell_epoch = 3;
  msg.state.position = {1.f, 2.f, 3.f};
  msg.state.velocity = {4.f, 5.f, 6.f};
  msg.state.direction = {0.f, 0.f, 1.f};
  msg.state.flags = movement::kMovementFlagGrounded;
  msg.state.last_processed_input_seq = 120;
  msg.correction_flags = 2;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 88u);
  EXPECT_EQ(rt->acked_input_seq, 120u);
  EXPECT_EQ(rt->cell_epoch, 3u);
  EXPECT_FLOAT_EQ(rt->state.position.z, 3.f);
  EXPECT_FLOAT_EQ(rt->state.velocity.y, 5.f);
  EXPECT_EQ(rt->state.flags, movement::kMovementFlagGrounded);
  EXPECT_EQ(rt->correction_flags, 2u);
  EXPECT_TRUE(MovementStateAckFromCell::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementStateAckToClientUsesDedicatedWireId) {
  MovementStateAckToClient msg;
  msg.entity_id = 88;
  msg.acked_input_seq = 121;
  msg.server_tick = 301;
  msg.state.position = {7.f, 8.f, 9.f};
  msg.state.last_processed_input_seq = 121;
  msg.correction_flags = 1;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 88u);
  EXPECT_FLOAT_EQ(rt->state.position.x, 7.f);
  EXPECT_EQ(MovementStateAckToClient::Descriptor().id,
            kClientMovementStateAckMessageId);
  EXPECT_NE(MovementStateAckToClient::Descriptor().id, kClientDeltaMessageId);
  EXPECT_TRUE(MovementStateAckToClient::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementStateAckToClientRejectsNonFiniteState) {
  MovementStateAckToClient msg;
  msg.entity_id = 88;
  msg.server_tick = 301;
  msg.state.position.x = std::numeric_limits<float>::quiet_NaN();

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementStateAckToClientRejectsInvalidEntity) {
  MovementStateAckToClient msg;
  msg.server_tick = 301;

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementStateAckRejectsInvalidCorrectionFlags) {
  MovementStateAckToClient client_msg;
  client_msg.entity_id = 88;
  client_msg.server_tick = 301;
  client_msg.state.direction = {0.f, 0.f, 1.f};
  client_msg.correction_flags = movement::kCorrectionFlagTier1 |
                                movement::kCorrectionFlagTier2;
  EXPECT_FALSE(round_trip(client_msg).has_value());
  client_msg.correction_flags = 1u << 7;
  EXPECT_FALSE(round_trip(client_msg).has_value());

  MovementStateAckFromCell cell_msg;
  cell_msg.entity_id = 88;
  cell_msg.server_tick = 301;
  cell_msg.cell_epoch = 1;
  cell_msg.state.direction = {0.f, 0.f, 1.f};
  cell_msg.correction_flags = movement::kCorrectionFlagTier2 |
                              movement::kCorrectionFlagSnap;
  EXPECT_FALSE(round_trip(cell_msg).has_value());
  cell_msg.correction_flags = 1u << 5;
  EXPECT_FALSE(round_trip(cell_msg).has_value());
}

TEST(BaseAppMessages, MovementCommandStartToClientUsesDedicatedWireId) {
  MovementCommandStartToClient msg;
  msg.entity_id = 89;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.type = movement::MovementCommandType::kKnockback;
  msg.command.start_position = {1.f, 2.f, 3.f};
  msg.command.target_position = {4.f, 5.f, 6.f};
  msg.command.duration_ms = 700;
  msg.command.elapsed_ms = 100;
  msg.command.curve_id = 3;
  msg.command.input_policy = movement::MovementCommandInputPolicy::kAllowTurn;
  msg.command.collision_policy = movement::MovementCommandCollisionPolicy::kEndSkill;
  msg.command.priority = 9;
  msg.command.server_tick = 301;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 89u);
  EXPECT_EQ(rt->command.command_id, 900u);
  EXPECT_EQ(rt->command.type, movement::MovementCommandType::kKnockback);
  EXPECT_FLOAT_EQ(rt->command.target_position.z, 6.f);
  EXPECT_EQ(rt->command.collision_policy,
            movement::MovementCommandCollisionPolicy::kEndSkill);
  EXPECT_EQ(MovementCommandStartToClient::Descriptor().id,
            kClientMovementCommandStartMessageId);
  EXPECT_FALSE(MovementCommandStartToClient::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementCommandStartToClientRejectsZeroCommandId) {
  MovementCommandStartToClient msg;
  msg.entity_id = 89;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandStartToClientRejectsInvalidEntity) {
  MovementCommandStartToClient msg;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandStartToClientRejectsNonFinitePosition) {
  MovementCommandStartToClient msg;
  msg.entity_id = 89;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;
  msg.command.target_position.x = std::numeric_limits<float>::infinity();

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandStartToClientRejectsInvalidTiming) {
  MovementCommandStartToClient msg;
  msg.entity_id = 89;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 0;
  msg.command.server_tick = 301;

  EXPECT_FALSE(round_trip(msg).has_value());

  msg.command.duration_ms = 700;
  msg.command.elapsed_ms = 701;

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandStartFromCellRoundTrip) {
  MovementCommandStartFromCell msg;
  msg.source_entity_id = 100;
  msg.cell_epoch = 3;
  msg.dest_entity_ids = {100, 101};
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.type = movement::MovementCommandType::kKnockback;
  msg.command.start_position = {1.f, 2.f, 3.f};
  msg.command.target_position = {4.f, 5.f, 6.f};
  msg.command.duration_ms = 700;
  msg.command.elapsed_ms = 20;
  msg.command.curve_id = 3;
  msg.command.input_policy = movement::MovementCommandInputPolicy::kAllowTurn;
  msg.command.collision_policy = movement::MovementCommandCollisionPolicy::kEndSkill;
  msg.command.priority = 9;
  msg.command.server_tick = 301;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->source_entity_id, 100u);
  EXPECT_EQ(rt->cell_epoch, 3u);
  ASSERT_EQ(rt->dest_entity_ids.size(), 2u);
  EXPECT_EQ(rt->dest_entity_ids[1], 101u);
  EXPECT_EQ(rt->command.command_id, 900u);
  EXPECT_EQ(rt->command.type, movement::MovementCommandType::kKnockback);
  EXPECT_EQ(rt->command.collision_policy,
            movement::MovementCommandCollisionPolicy::kEndSkill);
  EXPECT_EQ(MovementCommandStartFromCell::Descriptor().id,
            msg_id::Id(msg_id::BaseApp::kMovementCommandStartFromCell));
  EXPECT_FALSE(MovementCommandStartFromCell::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementCommandStartFromCellRejectsZeroCommandId) {
  MovementCommandStartFromCell msg;
  msg.source_entity_id = 100;
  msg.cell_epoch = 3;
  msg.dest_entity_ids = {100};
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandStartFromCellRejectsInvalidEntityIds) {
  MovementCommandStartFromCell msg;
  msg.cell_epoch = 3;
  msg.dest_entity_ids = {100};
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;

  EXPECT_FALSE(round_trip(msg).has_value());

  msg.source_entity_id = 100;
  msg.dest_entity_ids = {0};

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandEndToClientUsesDedicatedWireId) {
  MovementCommandEndToClient msg;
  msg.entity_id = 89;
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.reason = movement::MovementCommandEndReason::kCollision;
  msg.state.position = {7.f, 8.f, 9.f};
  msg.state.direction = {0.f, 0.f, 1.f};
  msg.state.flags = movement::kMovementFlagGrounded;
  msg.state.last_processed_input_seq = 77;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->entity_id, 89u);
  EXPECT_EQ(rt->command_id, 900u);
  EXPECT_EQ(rt->server_tick, 302u);
  EXPECT_EQ(rt->reason, movement::MovementCommandEndReason::kCollision);
  EXPECT_FLOAT_EQ(rt->state.position.z, 9.f);
  EXPECT_EQ(rt->state.last_processed_input_seq, 77u);
  EXPECT_EQ(MovementCommandEndToClient::Descriptor().id,
            kClientMovementCommandEndMessageId);
  EXPECT_FALSE(MovementCommandEndToClient::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementCommandEndToClientRejectsZeroCommandId) {
  MovementCommandEndToClient msg;
  msg.entity_id = 89;
  msg.server_tick = 302;
  msg.state.direction = {0.f, 0.f, 1.f};

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandEndToClientRejectsInvalidEntity) {
  MovementCommandEndToClient msg;
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.state.direction = {0.f, 0.f, 1.f};

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandEndToClientRejectsNonFiniteState) {
  MovementCommandEndToClient msg;
  msg.entity_id = 89;
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.state.velocity.z = std::numeric_limits<float>::quiet_NaN();

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandEndFromCellRoundTrip) {
  MovementCommandEndFromCell msg;
  msg.source_entity_id = 100;
  msg.cell_epoch = 3;
  msg.dest_entity_ids = {100, 101};
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.reason = movement::MovementCommandEndReason::kCancelled;
  msg.state.position = {7.f, 8.f, 9.f};
  msg.state.direction = {0.f, 0.f, 1.f};
  msg.state.flags = movement::kMovementFlagGrounded;
  msg.state.last_processed_input_seq = 77;

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->source_entity_id, 100u);
  EXPECT_EQ(rt->cell_epoch, 3u);
  ASSERT_EQ(rt->dest_entity_ids.size(), 2u);
  EXPECT_EQ(rt->dest_entity_ids[1], 101u);
  EXPECT_EQ(rt->command_id, 900u);
  EXPECT_EQ(rt->server_tick, 302u);
  EXPECT_EQ(rt->reason, movement::MovementCommandEndReason::kCancelled);
  EXPECT_FLOAT_EQ(rt->state.position.z, 9.f);
  EXPECT_EQ(MovementCommandEndFromCell::Descriptor().id,
            msg_id::Id(msg_id::BaseApp::kMovementCommandEndFromCell));
  EXPECT_FALSE(MovementCommandEndFromCell::Descriptor().IsUnreliable());
}

TEST(BaseAppMessages, MovementCommandEndFromCellRejectsZeroCommandId) {
  MovementCommandEndFromCell msg;
  msg.source_entity_id = 100;
  msg.cell_epoch = 3;
  msg.dest_entity_ids = {100};
  msg.server_tick = 302;
  msg.state.direction = {0.f, 0.f, 1.f};

  EXPECT_FALSE(round_trip(msg).has_value());
}

TEST(BaseAppMessages, MovementCommandEndFromCellRejectsInvalidEntityIds) {
  MovementCommandEndFromCell msg;
  msg.cell_epoch = 3;
  msg.dest_entity_ids = {100};
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.state.direction = {0.f, 0.f, 1.f};

  EXPECT_FALSE(round_trip(msg).has_value());

  msg.source_entity_id = 100;
  msg.dest_entity_ids = {0};

  EXPECT_FALSE(round_trip(msg).has_value());
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

// CellAppDeath carries per-space fallback hosts plus optional leaf bounds
// for position-based restore target selection.
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

TEST(BaseAppMessages, CellAppDeathRoundTrip_RehomeCells) {
  CellAppDeath msg;
  msg.dead_addr = Address(0x0A000001u, 40000);
  msg.rehomes.emplace_back(SpaceID{7}, Address(0x0A000002u, 40001));
  msg.rehome_cells.push_back(
      {SpaceID{7}, 1u, Address(0x0A000002u, 40001), CellBounds{-100.f, -50.f, 0.f, 50.f}});
  msg.rehome_cells.push_back(
      {SpaceID{7}, 2u, Address(0x0A000003u, 40002), CellBounds{0.f, -50.f, 100.f, 50.f}});

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  ASSERT_EQ(rt->rehome_cells.size(), 2u);
  EXPECT_EQ(rt->rehome_cells[0].cell_id, 1u);
  EXPECT_EQ(rt->rehome_cells[1].host_addr.Port(), 40002u);
  EXPECT_FLOAT_EQ(rt->rehome_cells[1].bounds.min_x, 0.f);

  const auto* left = FindRehomeCellForPosition(*rt, SpaceID{7}, {-10.f, 0.f, 0.f});
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(left->host_addr.Port(), 40001u);

  const auto* right = FindRehomeCellForPosition(*rt, SpaceID{7}, {10.f, 0.f, 0.f});
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(right->host_addr.Port(), 40002u);

  EXPECT_EQ(FindRehomeCellForPosition(*rt, SpaceID{8}, {10.f, 0.f, 0.f}), nullptr);
}

TEST(BaseAppMessages, SpaceBspGeometryRoundTrip_IncludesLoadAndEntityCount) {
  SpaceBspGeometry msg;
  msg.space_id = 42;
  msg.leaves.push_back({7, 3, -10.f, -20.f, 30.f, 40.f, 0.625f, 123});

  auto rt = round_trip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->space_id, 42u);
  ASSERT_EQ(rt->leaves.size(), 1u);
  EXPECT_EQ(rt->leaves[0].cell_id, 7u);
  EXPECT_EQ(rt->leaves[0].owner_index, 3u);
  EXPECT_FLOAT_EQ(rt->leaves[0].min_x, -10.f);
  EXPECT_FLOAT_EQ(rt->leaves[0].max_z, 40.f);
  EXPECT_FLOAT_EQ(rt->leaves[0].load, 0.625f);
  EXPECT_EQ(rt->leaves[0].entity_count, 123u);
}
