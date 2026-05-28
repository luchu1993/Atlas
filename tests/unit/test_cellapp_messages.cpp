#include <limits>
#include <optional>
#include <span>

#include <gtest/gtest.h>

#include "cellapp_messages.h"
#include "intercell_messages.h"
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
  msg.require_existing_ghost = true;
  msg.cellapp_death_restore = true;

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
  EXPECT_TRUE(rt->require_existing_ghost);
  EXPECT_TRUE(rt->cellapp_death_restore);
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
  EXPECT_FALSE(rt->require_existing_ghost);
  EXPECT_FALSE(rt->cellapp_death_restore);
}

TEST(CellAppMessages, CreateCellEntityRejectsTruncatedRestoreFlags) {
  CreateCellEntity msg;
  msg.entity_id = 1;
  msg.type_id = 1;
  msg.space_id = 1;

  BinaryWriter w;
  msg.Serialize(w);
  auto buf = w.Detach();
  ASSERT_FALSE(buf.empty());
  buf.pop_back();

  BinaryReader r(std::span<const std::byte>(buf.data(), buf.size()));
  auto rt = CreateCellEntity::Deserialize(r);
  EXPECT_FALSE(rt.HasValue());
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

TEST(CellAppMessages, ClientMovementInputForwardRoundTrip) {
  ClientMovementInputForward msg;
  msg.source_entity_id = 10;
  msg.target_entity_id = 10;
  msg.frames.push_back({20, 200, -127, 0, 4096, -4, 0, 16});
  msg.frames.push_back({21, 201, 0, 127, 8192, 1, movement::kInputButtonJump, 17});

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->source_entity_id, 10u);
  EXPECT_EQ(rt->target_entity_id, 10u);
  ASSERT_EQ(rt->frames.size(), 2u);
  EXPECT_EQ(rt->frames[0].move_x, -127);
  EXPECT_EQ(rt->frames[1].buttons, movement::kInputButtonJump);
  EXPECT_TRUE(ClientMovementInputForward::Descriptor().IsUnreliable());
}

TEST(CellAppMessages, ClientMovementInputForwardRejectsZeroFrames) {
  BinaryWriter w;
  w.Write<uint32_t>(10);
  w.Write<uint32_t>(10);
  w.Write<uint8_t>(0);

  auto buf = w.Detach();
  BinaryReader r(buf);
  auto result = ClientMovementInputForward::Deserialize(r);

  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CellAppMessages, ClientMovementInputForwardRejectsInvalidEntityIds) {
  ClientMovementInputForward msg;
  msg.source_entity_id = kInvalidEntityID;
  msg.target_entity_id = 10;
  msg.frames.push_back({20, 200, -127, 0, 4096, -4, 0, movement::kMinInputDtMs});
  EXPECT_FALSE(RoundTrip(msg).has_value());

  msg.source_entity_id = 10;
  msg.target_entity_id = kInvalidEntityID;
  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, ClientMovementInputForwardRejectsInvalidFrame) {
  ClientMovementInputForward msg;
  msg.source_entity_id = 10;
  msg.target_entity_id = 10;
  msg.frames.push_back({20, 200, -127, 0, 4096, -4, 0, 0});

  auto rt = RoundTrip(msg);

  EXPECT_FALSE(rt.has_value());
}

TEST(CellAppMessages, OffloadEntityCarriesMovementState) {
  OffloadEntity msg;
  msg.entity_id = 22;
  msg.type_id = 3;
  msg.space_id = 4;
  msg.position = {1.0f, 0.0f, 2.0f};
  msg.direction = {0.0f, 0.0f, 1.0f};
  msg.on_ground = true;
  msg.target_cell_id = 7;
  msg.geometry_version = 9;
  msg.has_movement_state = true;
  msg.movement_state.position = {1.0f, 0.0f, 2.0f};
  msg.movement_state.velocity = {0.5f, 0.0f, 3.0f};
  msg.movement_state.direction = {0.0f, 0.0f, 1.0f};
  msg.movement_state.flags = movement::kMovementFlagGrounded;
  msg.movement_state.last_processed_input_seq = 123;
  MovementPositionSample sample;
  sample.server_tick = 99;
  sample.state = msg.movement_state;
  sample.state.position = {1.0f, 0.0f, 1.5f};
  msg.movement_position_history.push_back(sample);
  msg.has_movement_command = true;
  msg.movement_command.command_id = 44;
  msg.movement_command.skill_id = 12;
  msg.movement_command.type = movement::MovementCommandType::kDash;
  msg.movement_command.start_position = msg.position;
  msg.movement_command.target_position = {4.0f, 0.0f, 2.0f};
  msg.movement_command.duration_ms = 500;
  msg.movement_command.elapsed_ms = 100;
  msg.movement_command.curve_id = 3;
  msg.movement_command.server_tick = 88;

  auto rt = RoundTrip(msg);

  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->has_movement_state);
  EXPECT_FLOAT_EQ(rt->movement_state.velocity.x, 0.5f);
  EXPECT_FLOAT_EQ(rt->movement_state.velocity.z, 3.0f);
  EXPECT_EQ(rt->movement_state.last_processed_input_seq, 123u);
  ASSERT_EQ(rt->movement_position_history.size(), 1u);
  EXPECT_EQ(rt->movement_position_history[0].server_tick, 99u);
  EXPECT_FLOAT_EQ(rt->movement_position_history[0].state.position.z, 1.5f);
  EXPECT_TRUE(rt->has_movement_command);
  EXPECT_EQ(rt->movement_command.command_id, 44u);
  EXPECT_EQ(rt->movement_command.elapsed_ms, 100u);
  EXPECT_FLOAT_EQ(rt->movement_command.target_position.x, 4.0f);
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

TEST(CellAppMessages, ClientRpcBroadcastRoundTrip) {
  ClientRpcBroadcast msg;
  msg.source_entity_id = 88;
  msg.rpc_id = 0x00040001;
  msg.target = 2;  // RpcTarget::kAll
  msg.payload = {std::byte{0xBE}, std::byte{0xEF}, std::byte{0x42}};
  msg.trace_id = 0xCAFE'BABE'DEAD'BEEFULL;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->source_entity_id, 88u);
  EXPECT_EQ(rt->rpc_id, 0x00040001u);
  EXPECT_EQ(rt->target, 2u);
  EXPECT_EQ(rt->payload.size(), 3u);
  EXPECT_EQ(rt->trace_id, 0xCAFE'BABE'DEAD'BEEFULL);
}

TEST(CellAppMessages, MovementCommandStartBroadcastRoundTrip) {
  MovementCommandStartBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.type = movement::MovementCommandType::kDash;
  msg.command.start_position = {1.f, 2.f, 3.f};
  msg.command.target_position = {4.f, 5.f, 6.f};
  msg.command.duration_ms = 700;
  msg.command.elapsed_ms = 100;
  msg.command.curve_id = 3;
  msg.command.input_policy = movement::MovementCommandInputPolicy::kAllowTurn;
  msg.command.collision_policy = movement::MovementCommandCollisionPolicy::kStop;
  msg.command.priority = 9;
  msg.command.server_tick = 301;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->source_entity_id, 88u);
  EXPECT_EQ(rt->cell_epoch, 4u);
  EXPECT_EQ(rt->command.command_id, 900u);
  EXPECT_EQ(rt->command.type, movement::MovementCommandType::kDash);
  EXPECT_FLOAT_EQ(rt->command.target_position.z, 6.f);
  EXPECT_EQ(rt->command.collision_policy,
            movement::MovementCommandCollisionPolicy::kStop);
}

TEST(CellAppMessages, MovementCommandStartBroadcastRejectsZeroCommandId) {
  MovementCommandStartBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;

  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, MovementCommandStartBroadcastRejectsInvalidSource) {
  MovementCommandStartBroadcast msg;
  msg.cell_epoch = 4;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;

  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, MovementCommandStartBroadcastRejectsNonFinitePosition) {
  MovementCommandStartBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 700;
  msg.command.server_tick = 301;
  msg.command.start_position.y = std::numeric_limits<float>::quiet_NaN();

  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, MovementCommandStartBroadcastRejectsInvalidTiming) {
  MovementCommandStartBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.duration_ms = 0;
  msg.command.server_tick = 301;

  EXPECT_FALSE(RoundTrip(msg).has_value());

  msg.command.duration_ms = 700;
  msg.command.elapsed_ms = 701;

  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, MovementCommandEndBroadcastRoundTrip) {
  MovementCommandEndBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.reason = movement::MovementCommandEndReason::kCollision;
  msg.state.position = {7.f, 8.f, 9.f};
  msg.state.direction = {0.f, 0.f, 1.f};
  msg.state.flags = movement::kMovementFlagGrounded;
  msg.state.last_processed_input_seq = 77;

  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->source_entity_id, 88u);
  EXPECT_EQ(rt->cell_epoch, 4u);
  EXPECT_EQ(rt->command_id, 900u);
  EXPECT_EQ(rt->server_tick, 302u);
  EXPECT_EQ(rt->reason, movement::MovementCommandEndReason::kCollision);
  EXPECT_FLOAT_EQ(rt->state.position.z, 9.f);
  EXPECT_EQ(rt->state.last_processed_input_seq, 77u);
}

TEST(CellAppMessages, MovementCommandEndBroadcastRejectsZeroCommandId) {
  MovementCommandEndBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.server_tick = 302;
  msg.state.direction = {0.f, 0.f, 1.f};

  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, MovementCommandEndBroadcastRejectsInvalidSource) {
  MovementCommandEndBroadcast msg;
  msg.cell_epoch = 4;
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.state.direction = {0.f, 0.f, 1.f};

  EXPECT_FALSE(RoundTrip(msg).has_value());
}

TEST(CellAppMessages, MovementCommandEndBroadcastRejectsNonFiniteState) {
  MovementCommandEndBroadcast msg;
  msg.source_entity_id = 88;
  msg.cell_epoch = 4;
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.state.direction.z = std::numeric_limits<float>::infinity();

  EXPECT_FALSE(RoundTrip(msg).has_value());
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

  // These share a subsystem but carry independent semantics.
  // Keep the message ids distinct.
  EXPECT_NE(SetAoIRadius::Descriptor().id, EnableWitness::Descriptor().id);
  EXPECT_NE(SetAoIRadius::Descriptor().id, DisableWitness::Descriptor().id);
  EXPECT_EQ(SetAoIRadius::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::CellApp::kSetAoIRadius)));
}

TEST(CellAppMessages, ClientAndInternalRpcHaveDistinctIds) {
  EXPECT_NE(ClientCellRpcForward::Descriptor().id, InternalCellRpc::Descriptor().id);
  EXPECT_EQ(ClientCellRpcForward::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::CellApp::kClientCellRpcForward)));
  EXPECT_EQ(InternalCellRpc::Descriptor().id,
            static_cast<MessageID>(msg_id::Id(msg_id::CellApp::kInternalCellRpc)));
}

// Every CellApp message id sits in the allocated 3000-3999 band.
// Runtime checks also exercise each message struct's Descriptor() path.
TEST(CellAppMessages, AllMessagesInAllocatedIdRange) {
  auto check = [](MessageID id) {
    EXPECT_GE(static_cast<uint16_t>(id), 3000u);
    EXPECT_LE(static_cast<uint16_t>(id), 3999u);
  };
  check(CreateCellEntity::Descriptor().id);
  check(DestroyCellEntity::Descriptor().id);
  check(ClientCellRpcForward::Descriptor().id);
  check(ClientMovementInputForward::Descriptor().id);
  check(InternalCellRpc::Descriptor().id);
  check(ClientRpcBroadcast::Descriptor().id);
  check(MovementCommandStartBroadcast::Descriptor().id);
  check(MovementCommandEndBroadcast::Descriptor().id);
  check(CreateSpace::Descriptor().id);
  check(DestroySpace::Descriptor().id);
  check(AvatarUpdate::Descriptor().id);
  check(EnableWitness::Descriptor().id);
  check(DisableWitness::Descriptor().id);
  check(SetAoIRadius::Descriptor().id);
}
