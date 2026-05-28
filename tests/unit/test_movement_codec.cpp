#include <vector>

#include <gtest/gtest.h>

#include "movement_sim/movement_codec.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::movement;

namespace {

auto SerializedBytes(const InputFrame& frame) -> std::vector<std::byte> {
  BinaryWriter w(kInputFrameWireBytes);
  SerializeInputFrame(w, frame);
  return std::vector<std::byte>(w.Data().begin(), w.Data().end());
}

auto SerializedBytes(const MovementState& state) -> std::vector<std::byte> {
  BinaryWriter w;
  SerializeMovementState(w, state);
  return std::vector<std::byte>(w.Data().begin(), w.Data().end());
}

auto SerializedBytes(const MovementCommand& command) -> std::vector<std::byte> {
  BinaryWriter w(kMovementCommandWireBytes);
  SerializeMovementCommand(w, command);
  return std::vector<std::byte>(w.Data().begin(), w.Data().end());
}

auto SampleCommand() -> MovementCommand {
  MovementCommand c;
  c.command_id = 7;
  c.skill_id = 11;
  c.type = MovementCommandType::kDash;
  c.start_position = {1.0f, 2.0f, 3.0f};
  c.target_position = {4.0f, 5.0f, 6.0f};
  c.duration_ms = 500;
  c.elapsed_ms = 100;
  c.curve_id = 2;
  c.input_policy = MovementCommandInputPolicy::kAllowTurn;
  c.collision_policy = MovementCommandCollisionPolicy::kStop;
  c.priority = 3;
  c.server_tick = 99;
  return c;
}

}  // namespace

TEST(MovementCodec, InputFrameRoundtripsExactly) {
  InputFrame frame;
  frame.seq = 42;
  frame.input_tick = 17;
  frame.move_x = -64;
  frame.move_z = 64;
  frame.view_yaw = 0xC000;
  frame.view_pitch = 12;
  frame.buttons = 0xABCD;
  frame.client_dt_ms = 33;

  auto bytes = SerializedBytes(frame);
  BinaryReader r(bytes);
  auto result = DeserializeInputFrame(r);

  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->seq, frame.seq);
  EXPECT_EQ(result->input_tick, frame.input_tick);
  EXPECT_EQ(result->move_x, frame.move_x);
  EXPECT_EQ(result->move_z, frame.move_z);
  EXPECT_EQ(result->view_yaw, frame.view_yaw);
  EXPECT_EQ(result->view_pitch, frame.view_pitch);
  EXPECT_EQ(result->buttons, frame.buttons);
  EXPECT_EQ(result->client_dt_ms, frame.client_dt_ms);
}

TEST(MovementCodec, InputFrameStationaryAxesRoundtrip) {
  InputFrame frame;
  frame.seq = 1;
  frame.input_tick = 1;
  frame.move_x = 0;
  frame.move_z = 0;
  frame.view_yaw = 0;
  frame.view_pitch = 0;
  frame.buttons = 0;
  frame.client_dt_ms = 33;

  auto bytes = SerializedBytes(frame);
  BinaryReader r(bytes);
  auto result = DeserializeInputFrame(r);

  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->move_x, 0);
  EXPECT_EQ(result->move_z, 0);
}

TEST(MovementCodec, InputFrameTruncatedReturnsError) {
  InputFrame frame;
  frame.seq = 1;
  frame.client_dt_ms = 33;
  auto bytes = SerializedBytes(frame);
  bytes.resize(bytes.size() - 1);

  BinaryReader r(bytes);
  EXPECT_FALSE(DeserializeInputFrame(r).HasValue());
}

TEST(MovementCodec, MovementStateRoundtripsExactly) {
  MovementState state;
  state.position = {1.0f, 2.0f, 3.0f};
  state.velocity = {4.0f, 5.0f, 6.0f};
  state.direction = {0.0f, 0.0f, 1.0f};
  state.flags = 0x80000001;
  state.last_processed_input_seq = 99;

  auto bytes = SerializedBytes(state);
  BinaryReader r(bytes);
  auto result = DeserializeMovementState(r);

  ASSERT_TRUE(result.HasValue());
  EXPECT_FLOAT_EQ(result->position.x, state.position.x);
  EXPECT_FLOAT_EQ(result->velocity.z, state.velocity.z);
  EXPECT_FLOAT_EQ(result->direction.z, state.direction.z);
  EXPECT_EQ(result->flags, state.flags);
  EXPECT_EQ(result->last_processed_input_seq, state.last_processed_input_seq);
}

TEST(MovementCodec, MovementStateRejectsNonFinite) {
  MovementState state;
  state.position = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
  state.direction = {0.0f, 0.0f, 1.0f};
  auto bytes = SerializedBytes(state);

  BinaryReader r(bytes);
  EXPECT_FALSE(DeserializeMovementState(r).HasValue());
}

TEST(MovementCodec, MovementCommandRoundtripsExactly) {
  auto command = SampleCommand();
  auto bytes = SerializedBytes(command);
  BinaryReader r(bytes);

  auto result = DeserializeMovementCommand(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->command_id, command.command_id);
  EXPECT_EQ(result->type, command.type);
  EXPECT_EQ(result->input_policy, command.input_policy);
  EXPECT_EQ(result->collision_policy, command.collision_policy);
  EXPECT_EQ(result->server_tick, command.server_tick);
  EXPECT_FLOAT_EQ(result->target_position.y, command.target_position.y);
}

TEST(MovementCodec, MovementCommandRejectsZeroCommandId) {
  auto command = SampleCommand();
  command.command_id = 0;
  auto bytes = SerializedBytes(command);
  BinaryReader r(bytes);

  EXPECT_FALSE(DeserializeMovementCommand(r).HasValue());
}

TEST(MovementCodec, MovementCommandRejectsZeroDuration) {
  auto command = SampleCommand();
  command.duration_ms = 0;
  auto bytes = SerializedBytes(command);
  BinaryReader r(bytes);

  EXPECT_FALSE(DeserializeMovementCommand(r).HasValue());
}

TEST(MovementCodec, MovementCommandRejectsElapsedPastDuration) {
  auto command = SampleCommand();
  command.duration_ms = 100;
  command.elapsed_ms = 200;
  auto bytes = SerializedBytes(command);
  BinaryReader r(bytes);

  EXPECT_FALSE(DeserializeMovementCommand(r).HasValue());
}

TEST(MovementCodec, MovementCommandRejectsInvalidTypeEnum) {
  auto command = SampleCommand();
  auto bytes = SerializedBytes(command);
  bytes[sizeof(uint32_t) + sizeof(uint16_t)] = std::byte{0xFF};
  BinaryReader r(bytes);

  EXPECT_FALSE(DeserializeMovementCommand(r).HasValue());
}

TEST(MovementCodec, MovementCommandRejectsNonFinitePosition) {
  auto command = SampleCommand();
  command.start_position = {std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
  auto bytes = SerializedBytes(command);
  BinaryReader r(bytes);

  EXPECT_FALSE(DeserializeMovementCommand(r).HasValue());
}

TEST(MovementCodec, MovementCommandTruncatedReturnsError) {
  auto command = SampleCommand();
  auto bytes = SerializedBytes(command);
  bytes.resize(bytes.size() - 1);
  BinaryReader r(bytes);

  EXPECT_FALSE(DeserializeMovementCommand(r).HasValue());
}
