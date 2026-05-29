#include "cell_movement_system.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "clrscript/native_api_provider.h"
#include "foundation/log.h"
#include "foundation/profiler.h"
#include "math/math_types.h"
#include "movement_sim/movement_codec.h"
#include "physics/physics_query.h"

namespace atlas {
namespace {

constexpr uint32_t kMaxMovementFramesPerEntityPerTick = 1;
constexpr uint32_t kMovementAckIntervalTicks = 3;
constexpr float kMovementIntentEpsilon = 1e-4f;

struct CommandDeltaResult {
  uint16_t advance_ms;
  float new_residue_seconds;
};

// Sub-millisecond dt accumulates into the residue and skips the command
// step this tick; once residue + dt >= 1ms the floor(total_ms) advances
// and the rounded-off fraction stays for the next call. Without this,
// every tick advanced elapsed_ms by 1 (the kFixed clamp) regardless of
// real dt — commands completed in a fraction of their nominal duration
// and the velocity calc (1000/dt_ms) was off proportionally.
auto MovementCommandDeltaMsWithResidue(float dt, float prev_residue_seconds)
    -> CommandDeltaResult {
  const double total_seconds = static_cast<double>(dt) + static_cast<double>(prev_residue_seconds);
  if (total_seconds < 0.001) {
    return {0u, static_cast<float>(total_seconds)};
  }
  const auto rounded_ms = std::lround(total_seconds * 1000.0);
  const auto clamped_ms = std::clamp<int64_t>(
      rounded_ms, 0, static_cast<int64_t>(std::numeric_limits<uint16_t>::max()));
  const auto advance_ms = static_cast<uint16_t>(clamped_ms);
  return {advance_ms, static_cast<float>(total_seconds - static_cast<double>(advance_ms) / 1000.0)};
}

auto QuantizeMovementAxis(float value) -> int8_t {
  return static_cast<int8_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
}

auto QuantizeMovementYaw(float dir_x, float dir_z) -> uint16_t {
  float yaw = std::atan2(dir_x, dir_z);
  if (yaw < 0.0f) yaw += math::kTwoPi;
  const float normalized = std::clamp(yaw / math::kTwoPi, 0.0f, 1.0f);
  return static_cast<uint16_t>(std::lround(normalized * 65535.0f));
}

}  // namespace

CellMovementSystem::CellMovementSystem(
    std::shared_ptr<const movement::MovementCommandResolver> command_resolver,
    std::shared_ptr<const movement::MovementCommandPolicy> command_policy)
    : command_resolver_(std::move(command_resolver)),
      command_policy_(std::move(command_policy)) {
  (void)curve_store_.Set(movement::MakeLinearMovementCurve(0));
}

void CellMovementSystem::RegisterWatchers(WatcherRegistry& wr) {
  metrics_.RegisterWatchers(
      wr, [this] { return input_buffer_.TotalQueueDepth(); },
      [this] { return position_history_.EntityCount(); },
      [this] { return position_history_.TotalSampleCount(); },
      [this] { return command_store_.Size(); });
}

void CellMovementSystem::SetIntent(CellMovementHost& host, EntityID entity_id, float dir_x,
                                   float dir_z, float speed_mps, uint16_t buttons) {
  if (!std::isfinite(dir_x) || !std::isfinite(dir_z) || !std::isfinite(speed_mps)) {
    metrics_.RecordInputDrop();
    return;
  }
  MovementActorSnapshot actor;
  if (!host.FindMovementActor(entity_id, actor)) return;

  movement::InputFrame input;
  input.buttons = buttons;
  const float max_speed = std::max(config_.max_speed_mps, 0.0f);
  const float clamped_speed =
      max_speed > kMovementIntentEpsilon ? std::clamp(speed_mps / max_speed, 0.0f, 1.0f) : 0.0f;
  const float len_sq = dir_x * dir_x + dir_z * dir_z;
  if (clamped_speed > 0.0f && len_sq > kMovementIntentEpsilon) {
    const float inv_len = 1.0f / std::sqrt(len_sq);
    const float unit_x = dir_x * inv_len;
    const float unit_z = dir_z * inv_len;
    input.move_z = QuantizeMovementAxis(clamped_speed);
    input.view_yaw = QuantizeMovementYaw(unit_x, unit_z);
  }
  script_intents_[entity_id] = input;
}

auto CellMovementSystem::SetCommand(CellMovementHost& host, EntityID entity_id,
                                    const movement::MovementCommand& command) -> bool {
  MovementActorSnapshot actor;
  if (!host.FindMovementActor(entity_id, actor)) return false;
  auto stamped = command;
  stamped.server_tick = host.MovementServerTick();
  if (curve_store_.Find(stamped.curve_id) == nullptr) return false;
  bool interrupted_command = false;
  uint32_t interrupted_command_id = 0;
  if (const auto* active = command_store_.Find(entity_id); active != nullptr) {
    if (active->command_id != stamped.command_id) {
      if (stamped.priority <= active->priority) return false;
      interrupted_command = true;
      interrupted_command_id = active->command_id;
    }
  }
  if (!command_store_.Set(entity_id, stamped)) {
    ATLAS_LOG_WARNING("CellApp: invalid movement command for entity_id={} - dropped", entity_id);
    return false;
  }
  if (interrupted_command) {
    auto& state =
        state_store_.Ensure(entity_id, actor.position, actor.direction, actor.on_ground);
    host.SendMovementCommandEnd(entity_id, interrupted_command_id, state,
                                stamped.server_tick,
                                movement::MovementCommandEndReason::kCancelled);
    metrics_.RecordCommandEnded(movement::MovementCommandEndReason::kCancelled);
  }
  if (command_policy_->SuppressesInput(stamped)) input_buffer_.Erase(entity_id);
  host.SendMovementCommandStart(entity_id, stamped);
  metrics_.RecordCommandStarted();
  return true;
}

auto CellMovementSystem::ClearCommand(CellMovementHost& host, EntityID entity_id,
                                      uint32_t command_id) -> bool {
  MovementActorSnapshot actor;
  if (!host.FindMovementActor(entity_id, actor)) return false;
  const auto* active = command_store_.Find(entity_id);
  if (active == nullptr) return false;
  if (command_id != 0 && active->command_id != command_id) return false;

  const uint32_t ended_command_id = active->command_id;
  auto& state =
      state_store_.Ensure(entity_id, actor.position, actor.direction, actor.on_ground);
  command_store_.Erase(entity_id);
  host.SendMovementCommandEnd(entity_id, ended_command_id, state, host.MovementServerTick(),
                              movement::MovementCommandEndReason::kCancelled);
  metrics_.RecordCommandEnded(movement::MovementCommandEndReason::kCancelled);
  return true;
}

auto CellMovementSystem::EnqueueClientInput(CellMovementHost& host, EntityID entity_id,
                                            std::span<const movement::InputFrame> frames)
    -> bool {
  if (frames.empty() || frames.size() > movement::kMaxMovementInputFrames) {
    metrics_.RecordInputDrop();
    return false;
  }
  for (const auto& frame : frames) {
    if (!movement::IsInputFrameValid(frame)) {
      metrics_.RecordInputInvalidDrop();
      return false;
    }
  }

  MovementActorSnapshot actor;
  if (!host.FindMovementActor(entity_id, actor)) {
    metrics_.RecordInputDrop();
    return false;
  }
  if (!ConsumeInputPacketToken(host, entity_id)) {
    metrics_.RecordInputDrop();
    return false;
  }
  if (const auto* command = command_store_.Find(entity_id);
      command != nullptr && curve_store_.Find(command->curve_id) != nullptr &&
      command_policy_->SuppressesInput(*command)) {
    metrics_.RecordInputDrop(frames.size());
    return false;
  }

  const auto result = input_buffer_.Enqueue(entity_id, frames);
  metrics_.RecordInputEnqueueResult(result);
  return result.accepted != 0;
}

void CellMovementSystem::RecordInputPacket() {
  metrics_.RecordInputPacket();
}

void CellMovementSystem::RecordInputDrop() {
  metrics_.RecordInputDrop();
}

void CellMovementSystem::RecordAckSent() {
  metrics_.RecordAckSent();
}

auto CellMovementSystem::RestoreState(EntityID entity_id,
                                      const movement::MovementState& state) -> bool {
  if (!movement::IsStateWithinLimits(state, config_)) {
    ATLAS_LOG_WARNING("CellApp: invalid movement state for entity_id={} - dropped", entity_id);
    command_store_.Erase(entity_id);
    position_history_.Erase(entity_id);
    state_store_.Erase(entity_id);
    return false;
  }
  const bool on_ground = (state.flags & movement::kMovementFlagGrounded) != 0;
  auto& stored = state_store_.Ensure(entity_id, state.position, state.direction, on_ground);
  stored = state;
  return true;
}

void CellMovementSystem::RestorePositionHistoryFromOffload(
    EntityID entity_id, uint32_t current_server_tick,
    std::span<const MovementPositionSample> samples) {
  position_history_.Erase(entity_id);
  if (samples.empty()) return;
  uint32_t source_max = 0;
  for (const auto& s : samples) {
    if (s.server_tick > source_max) source_max = s.server_tick;
  }
  const int64_t offset =
      static_cast<int64_t>(current_server_tick) - static_cast<int64_t>(source_max);
  for (const auto& sample : samples) {
    if (!movement::IsStateWithinLimits(sample.state, config_)) continue;
    const int64_t new_tick = static_cast<int64_t>(sample.server_tick) + offset;
    if (new_tick < 0 || new_tick > std::numeric_limits<uint32_t>::max()) continue;
    position_history_.Record(entity_id, static_cast<uint32_t>(new_tick), sample.state);
  }
}

void CellMovementSystem::RestorePositionHistoryAsIs(
    EntityID entity_id, std::span<const MovementPositionSample> samples) {
  position_history_.Erase(entity_id);
  for (const auto& sample : samples) {
    if (!movement::IsStateWithinLimits(sample.state, config_)) continue;
    position_history_.Record(entity_id, sample.server_tick, sample.state);
  }
}

auto CellMovementSystem::RestoreCommand(EntityID entity_id,
                                        const movement::MovementCommand& command) -> bool {
  if (!command_store_.Set(entity_id, command)) {
    ATLAS_LOG_WARNING("CellApp: invalid movement command for entity_id={} - dropped", entity_id);
    command_store_.Erase(entity_id);
    return false;
  }
  return true;
}

void CellMovementSystem::ClearStoredCommand(EntityID entity_id) {
  command_store_.Erase(entity_id);
}

void CellMovementSystem::EraseEntity(EntityID entity_id) {
  input_buffer_.Erase(entity_id);
  command_store_.Erase(entity_id);
  position_history_.Erase(entity_id);
  state_store_.Erase(entity_id);
  script_intents_.erase(entity_id);
  input_rate_limiter_.Erase(entity_id);
  command_dt_residue_seconds_.erase(entity_id);
}

auto CellMovementSystem::SetCurve(const movement::MovementCurve& curve) -> bool {
  return curve_store_.Set(curve);
}

auto CellMovementSystem::SampleHistory(EntityID entity_id, uint32_t server_tick,
                                       NativeMovementHistorySample& out) const -> bool {
  auto sample = position_history_.SampleAt(entity_id, server_tick);
  if (!sample) return false;
  out.server_tick = sample->server_tick;
  out.position_x = sample->state.position.x;
  out.position_y = sample->state.position.y;
  out.position_z = sample->state.position.z;
  out.velocity_x = sample->state.velocity.x;
  out.velocity_y = sample->state.velocity.y;
  out.velocity_z = sample->state.velocity.z;
  out.direction_x = sample->state.direction.x;
  out.direction_y = sample->state.direction.y;
  out.direction_z = sample->state.direction.z;
  out.flags = sample->state.flags;
  out.last_processed_input_seq = sample->state.last_processed_input_seq;
  return true;
}

void CellMovementSystem::CaptureOffloadState(EntityID entity_id, bool& has_state,
                                             movement::MovementState& state,
                                             std::vector<MovementPositionSample>& history,
                                             bool& has_command,
                                             movement::MovementCommand& command) const {
  has_state = false;
  has_command = false;
  history.clear();
  if (const auto* stored_state = state_store_.Find(entity_id);
      stored_state != nullptr && movement::IsStateWithinLimits(*stored_state, config_)) {
    has_state = true;
    state = *stored_state;
  }
  if (const auto* stored_history = position_history_.Find(entity_id)) {
    history.assign(stored_history->begin(), stored_history->end());
  }
  if (const auto* stored_command = command_store_.Find(entity_id)) {
    has_command = true;
    command = *stored_command;
  }
}

auto CellMovementSystem::ConsumeInputPacketToken(CellMovementHost& host, EntityID entity_id)
    -> bool {
  if (!input_rate_limiter_.Consume(entity_id, host.MovementNow())) {
    metrics_.RecordInputRateLimited();
    return false;
  }
  return true;
}

void CellMovementSystem::Tick(CellMovementHost& host, float dt) {
  ATLAS_PROFILE_ZONE_N("CellMovementSystem::Tick");
  const auto step_started_at = Clock::now();
  const auto record_step_time = [this, step_started_at] {
    metrics_.RecordStepTime(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - step_started_at));
  };
  if (!std::isfinite(dt) || dt <= 0.0f) {
    script_intents_.clear();
    record_step_time();
    return;
  }

  tick_entity_scratch_.clear();
  state_store_.AppendEntityIds(tick_entity_scratch_);
  input_buffer_.AppendEntityIdsWithPendingInput(tick_entity_scratch_);
  command_store_.AppendEntityIds(tick_entity_scratch_);
  for (const auto& [entity_id, _] : script_intents_) tick_entity_scratch_.push_back(entity_id);
  if (tick_entity_scratch_.empty()) {
    record_step_time();
    return;
  }

  std::sort(tick_entity_scratch_.begin(), tick_entity_scratch_.end());
  tick_entity_scratch_.erase(
      std::unique(tick_entity_scratch_.begin(), tick_entity_scratch_.end()),
      tick_entity_scratch_.end());

  auto config = config_;
  config.fixed_dt_s = dt;
  const uint32_t server_tick = host.MovementServerTick();
  const bool send_ack = (server_tick % kMovementAckIntervalTicks) == 0;

  for (EntityID entity_id : tick_entity_scratch_) {
    MovementActorSnapshot actor;
    if (!host.FindMovementActor(entity_id, actor) || actor.physics_query == nullptr) {
      EraseEntity(entity_id);
      continue;
    }

    auto& state =
        state_store_.Ensure(entity_id, actor.position, actor.direction, actor.on_ground);
    if (!movement::IsStateWithinLimits(state, config)) {
      EraseEntity(entity_id);
      metrics_.RecordInputDrop();
      continue;
    }

    movement::PhysicsCharacterQuery movement_query(*actor.physics_query, 2.0f,
                                                   physics::LayerMask{},
                                                   config.capsule_radius_m);
    if (auto* command = command_store_.Find(entity_id); command != nullptr) {
      const uint32_t command_id = command->command_id;
      const auto* curve = curve_store_.Find(command->curve_id);
      if (curve != nullptr) {
        std::optional<movement::InputFrame> turn_input;
        if (command_policy_->AllowsTurnInput(*command)) {
          auto frames =
              input_buffer_.Drain(entity_id, kMaxMovementFramesPerEntityPerTick);
          if (!frames.empty()) turn_input = frames.back();
        }
        const float prev_residue = command_dt_residue_seconds_[entity_id];
        const auto delta = MovementCommandDeltaMsWithResidue(dt, prev_residue);
        command_dt_residue_seconds_[entity_id] = delta.new_residue_seconds;
        if (delta.advance_ms == 0) {
          // Sub-ms tick — skip the command step; residue carries forward so
          // a subsequent tick can advance the command by the accumulated total.
          continue;
        }
        const auto result = movement::ApplyMovementCommand(
            state, *command, *curve, delta.advance_ms, config, movement_query,
            *command_resolver_, *command_policy_);
        if (movement::IsStateWithinLimits(result.state, config)) {
          auto next_state = result.state;
          if (turn_input.has_value()) {
            next_state.direction = movement::InputFacingDirection(*turn_input);
            next_state.last_processed_input_seq = turn_input->seq;
          }
          if (command_policy_->SuppressesInput(*command)) {
            input_buffer_.Erase(entity_id);
          }
          state = next_state;
          if (!(result.active && command_store_.Set(entity_id, result.command))) {
            command_store_.Erase(entity_id);
            const auto reason = command_policy_->EndReasonFor(result);
            host.SendMovementCommandEnd(entity_id, command_id, state, server_tick,
                                        reason);
            metrics_.RecordCommandEnded(reason);
          }
          host.PublishMovementState(entity_id, state);
          position_history_.Record(entity_id, server_tick, state);
          metrics_.RecordPositionHistorySample();
          if (send_ack) host.SendMovementStateAck(entity_id, state, server_tick);
          continue;
        }
      }
      command_store_.Erase(entity_id);
      host.SendMovementCommandEnd(entity_id, command_id, state, server_tick,
                                  movement::MovementCommandEndReason::kInvalid);
      metrics_.RecordCommandEnded(movement::MovementCommandEndReason::kInvalid);
    }

    auto frames = input_buffer_.Drain(entity_id, kMaxMovementFramesPerEntityPerTick);
    if (frames.empty()) {
      movement::InputFrame input;
      if (auto intent = script_intents_.find(entity_id); intent != script_intents_.end()) {
        input = intent->second;
      }
      input.seq = state.last_processed_input_seq;
      input.input_tick = server_tick;
      frames.push_back(input);
    }

    bool advanced = false;
    for (const auto& frame : frames) {
      const auto result = movement::Step(state, frame, config, movement_query, server_tick);
      if (!movement::IsStateWithinLimits(result.state, config) ||
          (!result.blocked &&
           !movement::IsHorizontalAccelerationWithinLimits(state, result.state, config))) {
        EraseEntity(entity_id);
        metrics_.RecordInputDrop();
        advanced = false;
        break;
      }
      state = result.state;
      metrics_.RecordFrameSimulated();
      advanced = true;
    }
    if (!advanced) continue;

    host.PublishMovementState(entity_id, state);
    position_history_.Record(entity_id, server_tick, state);
    metrics_.RecordPositionHistorySample();
    if (send_ack) host.SendMovementStateAck(entity_id, state, server_tick);
  }
  script_intents_.clear();
  record_step_time();
}

}  // namespace atlas
