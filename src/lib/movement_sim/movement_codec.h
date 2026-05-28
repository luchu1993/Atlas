#ifndef ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_CODEC_H_
#define ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_CODEC_H_

#include <cstddef>
#include <cstdint>

#include "movement_sim/movement_sim.h"
#include "serialization/binary_stream.h"

namespace atlas::movement {

inline constexpr uint8_t kMaxMovementInputFrames = 3;
inline constexpr std::size_t kInputFrameWireBytes = sizeof(uint32_t) + sizeof(uint32_t) +
                                                    sizeof(int8_t) + sizeof(int8_t) +
                                                    sizeof(uint16_t) + sizeof(int8_t) +
                                                    sizeof(uint16_t) + sizeof(uint16_t);
inline constexpr std::size_t kMovementCommandWireBytes =
    sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t) + 6 * sizeof(float) +
    3 * sizeof(uint16_t) + 3 * sizeof(uint8_t) + sizeof(uint32_t);
inline constexpr std::size_t kMovementCommandEndReasonWireBytes = sizeof(uint8_t);

inline void SerializeInputFrame(BinaryWriter& w, const InputFrame& frame) {
  w.Write(frame.seq);
  w.Write(frame.input_tick);
  w.Write(frame.move_x);
  w.Write(frame.move_z);
  w.Write(frame.view_yaw);
  w.Write(frame.view_pitch);
  w.Write(frame.buttons);
  w.Write(frame.client_dt_ms);
}

[[nodiscard]] inline auto DeserializeInputFrame(BinaryReader& r) -> Result<InputFrame> {
  auto seq = r.Read<uint32_t>();
  auto tick = r.Read<uint32_t>();
  auto move_x = r.Read<int8_t>();
  auto move_z = r.Read<int8_t>();
  auto yaw = r.Read<uint16_t>();
  auto pitch = r.Read<int8_t>();
  auto buttons = r.Read<uint16_t>();
  auto dt = r.Read<uint16_t>();
  if (!seq || !tick || !move_x || !move_z || !yaw || !pitch || !buttons || !dt) {
    return Error{ErrorCode::kInvalidArgument, "InputFrame: truncated"};
  }
  InputFrame frame;
  frame.seq = *seq;
  frame.input_tick = *tick;
  frame.move_x = *move_x;
  frame.move_z = *move_z;
  frame.view_yaw = *yaw;
  frame.view_pitch = *pitch;
  frame.buttons = *buttons;
  frame.client_dt_ms = *dt;
  return frame;
}

inline void SerializeMovementState(BinaryWriter& w, const MovementState& state) {
  w.Write(state.position.x);
  w.Write(state.position.y);
  w.Write(state.position.z);
  w.Write(state.velocity.x);
  w.Write(state.velocity.y);
  w.Write(state.velocity.z);
  w.Write(state.direction.x);
  w.Write(state.direction.y);
  w.Write(state.direction.z);
  w.Write(state.flags);
  w.Write(state.last_processed_input_seq);
}

[[nodiscard]] inline auto DeserializeMovementState(BinaryReader& r) -> Result<MovementState> {
  auto px = r.Read<float>();
  auto py = r.Read<float>();
  auto pz = r.Read<float>();
  auto vx = r.Read<float>();
  auto vy = r.Read<float>();
  auto vz = r.Read<float>();
  auto dx = r.Read<float>();
  auto dy = r.Read<float>();
  auto dz = r.Read<float>();
  auto flags = r.Read<uint32_t>();
  auto seq = r.Read<uint32_t>();
  if (!px || !py || !pz || !vx || !vy || !vz || !dx || !dy || !dz || !flags || !seq) {
    return Error{ErrorCode::kInvalidArgument, "MovementState: truncated"};
  }
  MovementState state;
  state.position = {*px, *py, *pz};
  state.velocity = {*vx, *vy, *vz};
  state.direction = {*dx, *dy, *dz};
  state.flags = *flags;
  state.last_processed_input_seq = *seq;
  if (!IsFinite(state)) {
    return Error{ErrorCode::kInvalidArgument, "MovementState: non-finite"};
  }
  return state;
}

inline void SerializeMovementCommand(BinaryWriter& w, const MovementCommand& command) {
  w.Write(command.command_id);
  w.Write(command.skill_id);
  w.Write(static_cast<uint8_t>(command.type));
  w.Write(command.start_position.x);
  w.Write(command.start_position.y);
  w.Write(command.start_position.z);
  w.Write(command.target_position.x);
  w.Write(command.target_position.y);
  w.Write(command.target_position.z);
  w.Write(command.duration_ms);
  w.Write(command.elapsed_ms);
  w.Write(command.curve_id);
  w.Write(static_cast<uint8_t>(command.input_policy));
  w.Write(static_cast<uint8_t>(command.collision_policy));
  w.Write(command.priority);
  w.Write(command.server_tick);
}

[[nodiscard]] inline auto IsMovementCommandTypeWireValue(uint8_t value) -> bool {
  return value <= static_cast<uint8_t>(MovementCommandType::kFollowEntity);
}

[[nodiscard]] inline auto IsMovementCommandInputPolicyWireValue(uint8_t value) -> bool {
  return value <= static_cast<uint8_t>(MovementCommandInputPolicy::kAllowFull);
}

[[nodiscard]] inline auto IsMovementCommandCollisionPolicyWireValue(uint8_t value) -> bool {
  return value <= static_cast<uint8_t>(MovementCommandCollisionPolicy::kEndSkill);
}

[[nodiscard]] inline auto IsMovementCommandEndReasonWireValue(uint8_t value) -> bool {
  return value <= static_cast<uint8_t>(MovementCommandEndReason::kInvalid);
}

[[nodiscard]] inline auto DeserializeMovementCommand(BinaryReader& r)
    -> Result<MovementCommand> {
  auto command_id = r.Read<uint32_t>();
  auto skill_id = r.Read<uint16_t>();
  auto type = r.Read<uint8_t>();
  auto sx = r.Read<float>();
  auto sy = r.Read<float>();
  auto sz = r.Read<float>();
  auto tx = r.Read<float>();
  auto ty = r.Read<float>();
  auto tz = r.Read<float>();
  auto duration = r.Read<uint16_t>();
  auto elapsed = r.Read<uint16_t>();
  auto curve_id = r.Read<uint16_t>();
  auto input_policy = r.Read<uint8_t>();
  auto collision_policy = r.Read<uint8_t>();
  auto priority = r.Read<uint8_t>();
  auto server_tick = r.Read<uint32_t>();
  if (!command_id || !skill_id || !type || !sx || !sy || !sz || !tx || !ty || !tz ||
      !duration || !elapsed || !curve_id || !input_policy || !collision_policy ||
      !priority || !server_tick) {
    return Error{ErrorCode::kInvalidArgument, "MovementCommand: truncated"};
  }
  if (*command_id == 0) {
    return Error{ErrorCode::kInvalidArgument, "MovementCommand: invalid command id"};
  }
  if (*duration == 0 || *elapsed > *duration) {
    return Error{ErrorCode::kInvalidArgument, "MovementCommand: invalid timing"};
  }
  if (!IsMovementCommandTypeWireValue(*type) ||
      !IsMovementCommandInputPolicyWireValue(*input_policy) ||
      !IsMovementCommandCollisionPolicyWireValue(*collision_policy)) {
    return Error{ErrorCode::kInvalidArgument, "MovementCommand: invalid enum"};
  }
  if (!IsFinite(math::Vector3{*sx, *sy, *sz}) ||
      !IsFinite(math::Vector3{*tx, *ty, *tz})) {
    return Error{ErrorCode::kInvalidArgument, "MovementCommand: non-finite position"};
  }

  MovementCommand command;
  command.command_id = *command_id;
  command.skill_id = *skill_id;
  command.type = static_cast<MovementCommandType>(*type);
  command.start_position = {*sx, *sy, *sz};
  command.target_position = {*tx, *ty, *tz};
  command.duration_ms = *duration;
  command.elapsed_ms = *elapsed;
  command.curve_id = *curve_id;
  command.input_policy = static_cast<MovementCommandInputPolicy>(*input_policy);
  command.collision_policy = static_cast<MovementCommandCollisionPolicy>(*collision_policy);
  command.priority = *priority;
  command.server_tick = *server_tick;
  return command;
}

}  // namespace atlas::movement

#endif  // ATLAS_LIB_MOVEMENT_SIM_MOVEMENT_CODEC_H_
