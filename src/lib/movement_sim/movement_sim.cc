#include "movement_sim/movement_sim.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "math/math_types.h"

namespace atlas::movement {
namespace {

[[nodiscard]] auto DecodeAxis(int8_t value) -> float {
  return static_cast<float>(std::clamp(static_cast<int>(value), -127, 127)) / 127.0f;
}

[[nodiscard]] auto DecodeYaw(uint16_t value) -> float {
  return (static_cast<float>(value) / 65535.0f) * math::kTwoPi;
}

[[nodiscard]] auto NonNegativeFinite(float value) -> float {
  return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

[[nodiscard]] auto Horizontal(const math::Vector3& v) -> math::Vector3 {
  return {v.x, 0.0f, v.z};
}

[[nodiscard]] auto ClampLength(const math::Vector3& v, float max_length) -> math::Vector3 {
  const float length = v.Length();
  if (length <= max_length || length <= math::kEpsilon) return v;
  return v * (max_length / length);
}

[[nodiscard]] auto SafeDirection(const math::Vector3& v) -> math::Vector3 {
  auto horizontal = Horizontal(v);
  if (horizontal.LengthSquared() <= math::kEpsilon) return {0.0f, 0.0f, 1.0f};
  return horizontal.Normalized();
}

void ClearGrounded(MovementState& state) {
  state.flags &= ~kMovementFlagGrounded;
}

void SetGrounded(MovementState& state) {
  state.flags |= kMovementFlagGrounded;
}

[[nodiscard]] auto WalkableGroundNormalY(const MovementConfig& config) -> float {
  if (!std::isfinite(config.max_walkable_slope_degrees)) return 1.0f;
  const float degrees = std::clamp(config.max_walkable_slope_degrees, 0.0f, 89.0f);
  return std::cos(degrees * math::kPi / 180.0f);
}

[[nodiscard]] auto IsWalkableGround(const GroundHit& hit, const MovementConfig& config) -> bool {
  if (!hit.hit || !IsFinite(hit.normal) || hit.normal.LengthSquared() <= math::kEpsilon) {
    return false;
  }
  return hit.normal.Normalized().y >= WalkableGroundNormalY(config);
}

[[nodiscard]] auto TryStepUp(const math::Vector3& start_position,
                             const math::Vector3& displacement,
                             const Capsule& capsule, const MovementConfig& config,
                             const CharacterQuery& query, math::Vector3* stepped_position)
    -> bool {
  const float step_height = NonNegativeFinite(config.step_height_m);
  const auto horizontal_displacement = Horizontal(displacement);
  if (step_height <= 0.0f ||
      horizontal_displacement.LengthSquared() <= math::kEpsilon ||
      stepped_position == nullptr) {
    return false;
  }

  Capsule raised_capsule = capsule;
  raised_capsule.center.y += step_height;
  if (query.OverlapCapsule(raised_capsule)) return false;
  if (query.SweepCapsule({raised_capsule, horizontal_displacement}).hit) return false;

  math::Vector3 candidate = start_position + horizontal_displacement;
  candidate.y += step_height;
  const auto ground = query.GroundProbe(candidate);
  if (!IsWalkableGround(ground, config)) return false;

  const float drop_distance = candidate.y - ground.position.y;
  const float snap = NonNegativeFinite(config.ground_snap_distance_m);
  if (drop_distance < 0.0f || drop_distance > step_height + snap) return false;

  *stepped_position = {candidate.x, ground.position.y, candidate.z};
  return true;
}

[[nodiscard]] auto TrySnapToGround(MovementState* state, const GroundHit& ground,
                                   const MovementConfig& config, bool* snapped) -> bool {
  if (state == nullptr || snapped == nullptr || state->velocity.y > 0.0f ||
      !IsWalkableGround(ground, config)) {
    return false;
  }

  const float snap = NonNegativeFinite(config.ground_snap_distance_m);
  if (state->position.y > ground.position.y + snap) return false;
  if (state->position.y < ground.position.y - math::kEpsilon) return false;

  *snapped = std::fabs(state->position.y - ground.position.y) > math::kEpsilon;
  state->position.y = ground.position.y;
  state->velocity.y = 0.0f;
  SetGrounded(*state);
  return true;
}

constexpr float kSurfaceOffsetM = 0.001f;

void ClipVelocityAgainstSurface(MovementState* state, const math::Vector3& normal) {
  if (state == nullptr || !IsFinite(normal) || normal.LengthSquared() <= math::kEpsilon) {
    return;
  }

  const auto surface_normal = normal.Normalized();
  const float into_surface = state->velocity.Dot(surface_normal);
  if (into_surface < 0.0f) state->velocity -= surface_normal * into_surface;
}

[[nodiscard]] auto SurfaceOffset(const math::Vector3& normal) -> math::Vector3 {
  if (!IsFinite(normal) || normal.LengthSquared() <= math::kEpsilon) return {};
  return normal.Normalized() * kSurfaceOffsetM;
}

[[nodiscard]] auto TryDepenetrate(MovementState* state, const Capsule& capsule,
                                  const MovementConfig& config,
                                  const CharacterQuery& query) -> bool {
  if (state == nullptr) return false;
  const auto hit = query.DepenetrateCapsule(capsule);
  if (!hit.hit || !IsFinite(hit.offset)) return false;

  const auto offset = ClampLength(hit.offset, NonNegativeFinite(config.max_depenetration_m));
  if (offset.LengthSquared() <= math::kEpsilon) return false;
  state->position += offset;
  ClipVelocityAgainstSurface(state, hit.normal);
  return true;
}

[[nodiscard]] auto DesiredMove(const InputFrame& input) -> math::Vector3 {
  const auto forward = InputFacingDirection(input);
  const math::Vector3 right{forward.z, 0.0f, -forward.x};
  auto desired = right * DecodeAxis(input.move_x) + forward * DecodeAxis(input.move_z);
  if (desired.LengthSquared() > 1.0f) desired = desired.Normalized();
  return desired;
}

[[nodiscard]] auto MoveTowards(const math::Vector3& from, const math::Vector3& to,
                               float max_delta) -> math::Vector3 {
  const auto delta = to - from;
  const float distance = delta.Length();
  if (distance <= max_delta || distance <= math::kEpsilon) return to;
  return from + delta * (max_delta / distance);
}

[[nodiscard]] auto Lerp(const math::Vector3& from, const math::Vector3& to,
                        float t) -> math::Vector3 {
  return from * (1.0f - t) + to * t;
}

[[nodiscard]] auto CommandCapsule(const MovementState& state,
                                  const MovementConfig& config) -> Capsule {
  Capsule capsule;
  capsule.center = state.position;
  capsule.radius_m = NonNegativeFinite(config.capsule_radius_m);
  capsule.half_height_m = NonNegativeFinite(config.capsule_half_height_m);
  return capsule;
}

[[nodiscard]] auto IsConfigFinite(const MovementConfig& config) -> bool {
  return std::isfinite(config.fixed_dt_s) && std::isfinite(config.max_speed_mps) &&
         std::isfinite(config.acceleration_mps2) &&
         std::isfinite(config.deceleration_mps2) && std::isfinite(config.gravity_mps2) &&
         std::isfinite(config.jump_speed_mps) && std::isfinite(config.max_fall_speed_mps) &&
         std::isfinite(config.max_position_abs_m) &&
         std::isfinite(config.capsule_radius_m) &&
         std::isfinite(config.capsule_half_height_m) &&
         std::isfinite(config.ground_snap_distance_m) &&
         std::isfinite(config.max_walkable_slope_degrees) &&
         std::isfinite(config.step_height_m) &&
         std::isfinite(config.max_depenetration_m);
}

constexpr float kVelocitySlackMps = 0.05f;

}

auto FlatGroundQuery::GroundProbe(const math::Vector3& position) const -> GroundHit {
  GroundHit hit;
  hit.hit = true;
  hit.position = {position.x, ground_y_, position.z};
  hit.distance_m = position.y - ground_y_;
  return hit;
}

auto FlatGroundQuery::SweepCapsule(const CapsuleCast& cast) const -> SweepHit {
  SweepHit hit;
  if (!IsFinite(cast.capsule.center) || !IsFinite(cast.displacement) ||
      cast.displacement.y >= -math::kEpsilon) {
    return hit;
  }

  const float start_y = cast.capsule.center.y;
  const float end_y = start_y + cast.displacement.y;
  if (start_y <= ground_y_ + math::kEpsilon || end_y > ground_y_) return hit;

  hit.hit = true;
  hit.fraction = std::clamp((ground_y_ - start_y) / cast.displacement.y, 0.0f, 1.0f);
  hit.normal = {0.0f, 1.0f, 0.0f};
  return hit;
}

auto FlatGroundQuery::OverlapCapsule(const Capsule& capsule) const -> bool {
  return IsFinite(capsule.center) && capsule.center.y < ground_y_ - math::kEpsilon;
}

auto FlatGroundQuery::DepenetrateCapsule(const Capsule& capsule) const -> DepenetrationHit {
  DepenetrationHit hit;
  if (!IsFinite(capsule.center)) return hit;

  const float depth = ground_y_ - capsule.center.y;
  if (depth <= math::kEpsilon) return hit;

  hit.hit = true;
  hit.offset = {0.0f, depth, 0.0f};
  hit.normal = {0.0f, 1.0f, 0.0f};
  hit.depth_m = depth;
  return hit;
}

auto CharacterQuery::DepenetrateCapsule(const Capsule&) const -> DepenetrationHit {
  return {};
}

PhysicsCharacterQuery::PhysicsCharacterQuery(const physics::PhysicsQuery& query,
                                             float ground_probe_distance_m,
                                             physics::LayerMask mask,
                                             float ground_probe_radius_m)
    : query_(query),
      ground_probe_distance_m_(ground_probe_distance_m),
      ground_probe_radius_m_(ground_probe_radius_m),
      mask_(mask) {}

auto PhysicsCharacterQuery::GroundProbe(const math::Vector3& position) const -> GroundHit {
  const auto hit = query_.GroundProbe(
      physics::GroundProbeQuery{position, NonNegativeFinite(ground_probe_distance_m_),
                                NonNegativeFinite(ground_probe_radius_m_),
                                physics::QueryFilter{mask_}});
  GroundHit out;
  out.hit = hit.hit;
  out.position = hit.position;
  out.normal = hit.normal;
  out.distance_m = hit.distance_m;
  return out;
}

auto PhysicsCharacterQuery::SweepCapsule(const CapsuleCast& cast) const -> SweepHit {
  physics::Capsule capsule;
  capsule.center = cast.capsule.center;
  capsule.radius_m = cast.capsule.radius_m;
  capsule.half_height_m = cast.capsule.half_height_m;
  const auto hit = query_.CastCapsule(
      physics::CapsuleCastQuery{capsule, cast.displacement, physics::QueryFilter{mask_}});
  SweepHit out;
  out.hit = hit.hit;
  out.fraction = hit.fraction;
  out.normal = hit.normal;
  return out;
}

auto PhysicsCharacterQuery::OverlapCapsule(const Capsule& capsule) const -> bool {
  physics::Capsule query_capsule;
  query_capsule.center = capsule.center;
  query_capsule.radius_m = capsule.radius_m;
  query_capsule.half_height_m = capsule.half_height_m;
  return query_.OverlapCapsule(physics::OverlapQuery{query_capsule, physics::QueryFilter{mask_}});
}

auto PhysicsCharacterQuery::DepenetrateCapsule(const Capsule& capsule) const
    -> DepenetrationHit {
  physics::Capsule query_capsule;
  query_capsule.center = capsule.center;
  query_capsule.radius_m = capsule.radius_m;
  query_capsule.half_height_m = capsule.half_height_m;
  const auto hit =
      query_.DepenetrateCapsule(physics::OverlapQuery{query_capsule, physics::QueryFilter{mask_}});
  DepenetrationHit out;
  out.hit = hit.hit;
  out.offset = hit.offset;
  out.normal = hit.normal;
  out.depth_m = hit.depth_m;
  return out;
}

auto IsFinite(const math::Vector3& v) -> bool {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

auto IsFinite(const MovementState& state) -> bool {
  return IsFinite(state.position) && IsFinite(state.velocity) && IsFinite(state.direction);
}

auto IsStateWithinLimits(const MovementState& state, const MovementConfig& config) -> bool {
  if (!IsFinite(state) || !IsConfigFinite(config) || config.max_position_abs_m <= 0.0f) {
    return false;
  }
  const float position_limit = config.max_position_abs_m;
  if (std::fabs(state.position.x) > position_limit ||
      std::fabs(state.position.y) > position_limit ||
      std::fabs(state.position.z) > position_limit) {
    return false;
  }

  const float max_horizontal_speed = NonNegativeFinite(config.max_speed_mps) + kVelocitySlackMps;
  if (Horizontal(state.velocity).LengthSquared() > max_horizontal_speed * max_horizontal_speed) {
    return false;
  }

  const float max_up_speed = NonNegativeFinite(config.jump_speed_mps) + kVelocitySlackMps;
  const float max_down_speed = NonNegativeFinite(config.max_fall_speed_mps) + kVelocitySlackMps;
  return state.velocity.y <= max_up_speed && state.velocity.y >= -max_down_speed;
}

auto IsHorizontalAccelerationWithinLimits(const MovementState& previous,
                                          const MovementState& current,
                                          const MovementConfig& config) -> bool {
  if (!IsFinite(previous) || !IsFinite(current) || !IsConfigFinite(config)) return false;
  const float dt = NonNegativeFinite(config.fixed_dt_s);
  if (dt <= 0.0f) return false;

  const float max_accel = std::max(NonNegativeFinite(config.acceleration_mps2),
                                   NonNegativeFinite(config.deceleration_mps2));
  const float max_delta = max_accel * dt + kVelocitySlackMps;
  const auto delta = Horizontal(current.velocity - previous.velocity);
  return delta.LengthSquared() <= max_delta * max_delta;
}

auto IsInputFrameValid(const InputFrame& input) -> bool {
  return input.client_dt_ms >= kMinInputDtMs && input.client_dt_ms <= kMaxInputDtMs;
}

auto InputFacingDirection(const InputFrame& input) -> math::Vector3 {
  const float yaw = DecodeYaw(input.view_yaw);
  return {std::sin(yaw), 0.0f, std::cos(yaw)};
}

auto InputSequenceDelta(uint32_t seq, uint32_t previous) -> uint32_t {
  return seq - previous;
}

auto IsInputSequenceNewer(uint32_t seq, uint32_t previous) -> bool {
  const uint32_t delta = InputSequenceDelta(seq, previous);
  return delta != 0 && delta < 0x80000000u;
}

auto ClassifyCorrection(float distance_m) -> CorrectionTier {
  if (!std::isfinite(distance_m) || distance_m < kCorrectionTier1DistanceM) {
    return CorrectionTier::kNone;
  }
  if (distance_m >= kCorrectionSnapDistanceM) return CorrectionTier::kSnap;
  if (distance_m >= kCorrectionTier2DistanceM) return CorrectionTier::kTier2;
  return CorrectionTier::kTier1;
}

auto CorrectionFlagForTier(CorrectionTier tier) -> uint16_t {
  switch (tier) {
    case CorrectionTier::kNone:
      return 0;
    case CorrectionTier::kTier1:
      return kCorrectionFlagTier1;
    case CorrectionTier::kTier2:
      return kCorrectionFlagTier2;
    case CorrectionTier::kSnap:
      return kCorrectionFlagSnap;
  }
  return 0;
}

auto IsCorrectionFlagsValid(uint16_t flags) -> bool {
  switch (flags) {
    case 0:
    case kCorrectionFlagTier1:
    case kCorrectionFlagTier2:
    case kCorrectionFlagSnap:
      return true;
    default:
      return false;
  }
}

auto IsMovementCurveValid(const MovementCurve& curve) -> bool {
  if (curve.sample_count == 0 || curve.sample_count > kMaxMovementCurveSamples) {
    return false;
  }
  for (std::size_t i = 0; i < curve.sample_count; ++i) {
    if (!std::isfinite(curve.samples[i])) return false;
  }
  return true;
}

auto IsMovementCommandValid(const MovementCommand& command,
                            const MovementCurve& curve) -> bool {
  if (command.command_id == 0 || command.duration_ms == 0 ||
      command.elapsed_ms > command.duration_ms || command.curve_id != curve.id) {
    return false;
  }
  return IsMovementCurveValid(curve) && IsFinite(command.start_position) &&
         IsFinite(command.target_position);
}

auto SampleMovementCurve(const MovementCurve& curve, float normalized_time) -> float {
  if (!IsMovementCurveValid(curve) || !std::isfinite(normalized_time)) return 0.0f;
  if (curve.sample_count == 1) return curve.samples[0];

  const float t = std::clamp(normalized_time, 0.0f, 1.0f);
  const float scaled = t * static_cast<float>(curve.sample_count - 1);
  const auto index = static_cast<std::size_t>(std::floor(scaled));
  if (index + 1 >= curve.sample_count) return curve.samples[curve.sample_count - 1];

  const float fraction = scaled - static_cast<float>(index);
  return curve.samples[index] * (1.0f - fraction) + curve.samples[index + 1] * fraction;
}

namespace {

class KinematicMovementCommandSampler final : public MovementCommandSampler {
 public:
  [[nodiscard]] auto Supports(MovementCommandType type) const -> bool override {
    switch (type) {
      case MovementCommandType::kDash:
      case MovementCommandType::kLaunch:
      case MovementCommandType::kLaunchOther:
      case MovementCommandType::kPull:
      case MovementCommandType::kKnockback:
      case MovementCommandType::kTeleport:
      case MovementCommandType::kFollowEntity:
        return true;
    }
    return false;
  }

  [[nodiscard]] auto Step(const MovementState& previous,
                          const MovementCommand& command,
                          const MovementCurve& curve,
                          uint16_t dt_ms) const -> MovementCommandStepResult override {
    MovementCommandStepResult result;
    result.state = previous;
    result.command = command;
    const uint32_t elapsed =
        std::min<uint32_t>(command.duration_ms, uint32_t{command.elapsed_ms} + dt_ms);
    const float normalized_time =
        static_cast<float>(elapsed) / static_cast<float>(command.duration_ms);
    const float progress = SampleMovementCurve(curve, normalized_time);

    result.state.position = Lerp(command.start_position, command.target_position, progress);
    if (dt_ms > 0) {
      const float inv_dt = 1000.0f / static_cast<float>(dt_ms);
      result.state.velocity = (result.state.position - previous.position) * inv_dt;
    } else {
      result.state.velocity = {};
    }

    const auto command_direction = command.target_position - command.start_position;
    if (command_direction.LengthSquared() > math::kEpsilon) {
      result.state.direction = SafeDirection(command_direction);
    }

    result.command.elapsed_ms = static_cast<uint16_t>(elapsed);
    result.active = elapsed < command.duration_ms;
    result.completed = !result.active;
    return result;
  }
};

class DefaultMovementCommandResolverImpl final : public MovementCommandResolver {
 public:
  [[nodiscard]] auto Find(MovementCommandType type) const
      -> const MovementCommandSampler* override {
    return sampler_.Supports(type) ? &sampler_ : nullptr;
  }

 private:
  KinematicMovementCommandSampler sampler_;
};

class DefaultMovementCommandPolicyImpl final : public MovementCommandPolicy {
 public:
  [[nodiscard]] auto SuppressesInput(const MovementCommand& command) const
      -> bool override {
    return command.input_policy == MovementCommandInputPolicy::kSuppress;
  }

  [[nodiscard]] auto AllowsTurnInput(const MovementCommand& command) const
      -> bool override {
    return command.input_policy == MovementCommandInputPolicy::kAllowTurn;
  }

  [[nodiscard]] auto ChecksCollision(const MovementCommand& command) const
      -> bool override {
    return command.collision_policy != MovementCommandCollisionPolicy::kContinue;
  }

  [[nodiscard]] auto EndReasonFor(const MovementCommandStepResult& result) const
      -> MovementCommandEndReason override {
    if (result.collision_ended) return MovementCommandEndReason::kCollision;
    if (result.completed) return MovementCommandEndReason::kCompleted;
    return MovementCommandEndReason::kInvalid;
  }
};

[[nodiscard]] auto CanApplyMovementCommand(const MovementCommand& command,
                                           const MovementCurve& curve,
                                           const MovementCommandResolver& resolver) -> bool {
  const auto* sampler = resolver.Find(command.type);
  return sampler != nullptr && sampler->Supports(command.type) &&
         IsMovementCommandValid(command, curve);
}

}

auto DefaultMovementCommandResolver() -> const MovementCommandResolver& {
  return *DefaultMovementCommandResolverPtr();
}

auto DefaultMovementCommandResolverPtr() -> std::shared_ptr<const MovementCommandResolver> {
  static const auto resolver = std::make_shared<DefaultMovementCommandResolverImpl>();
  return resolver;
}

auto DefaultMovementCommandPolicy() -> const MovementCommandPolicy& {
  return *DefaultMovementCommandPolicyPtr();
}

auto DefaultMovementCommandPolicyPtr() -> std::shared_ptr<const MovementCommandPolicy> {
  static const auto policy = std::make_shared<DefaultMovementCommandPolicyImpl>();
  return policy;
}

auto ApplyMovementCommand(const MovementState& previous,
                          const MovementCommand& command,
                          const MovementCurve& curve,
                          uint16_t dt_ms,
                          const MovementCommandResolver& resolver)
    -> MovementCommandStepResult {
  MovementCommandStepResult result;
  result.state = previous;
  result.command = command;
  const auto* sampler = resolver.Find(command.type);
  if (sampler == nullptr || !sampler->Supports(command.type) ||
      !IsMovementCommandValid(command, curve)) {
    result.completed = true;
    return result;
  }
  return sampler->Step(previous, command, curve, dt_ms);
}

auto ApplyMovementCommand(const MovementState& previous,
                          const MovementCommand& command,
                          const MovementCurve& curve, uint16_t dt_ms,
                          const MovementConfig& config,
                          const CharacterQuery& query,
                          const MovementCommandResolver& resolver)
    -> MovementCommandStepResult {
  return ApplyMovementCommand(previous, command, curve, dt_ms, config, query, resolver,
                              DefaultMovementCommandPolicy());
}

auto ApplyMovementCommand(const MovementState& previous,
                          const MovementCommand& command,
                          const MovementCurve& curve, uint16_t dt_ms,
                          const MovementConfig& config,
                          const CharacterQuery& query,
                          const MovementCommandResolver& resolver,
                          const MovementCommandPolicy& policy)
    -> MovementCommandStepResult {
  auto result = ApplyMovementCommand(previous, command, curve, dt_ms, resolver);
  if (!CanApplyMovementCommand(command, curve, resolver) ||
      !policy.ChecksCollision(command)) {
    return result;
  }

  const auto displacement = result.state.position - previous.position;
  if (displacement.LengthSquared() <= math::kEpsilon) return result;

  const auto hit = query.SweepCapsule({CommandCapsule(previous, config), displacement});
  if (!hit.hit) return result;

  const float fraction = std::clamp(hit.fraction, 0.0f, 1.0f);
  result.state.position = previous.position + displacement * fraction;
  result.state.velocity = {};
  result.command.elapsed_ms = static_cast<uint16_t>(std::min<uint32_t>(
      command.duration_ms,
      uint32_t{command.elapsed_ms} +
          static_cast<uint32_t>(std::lround(static_cast<float>(dt_ms) * fraction))));
  result.active = false;
  result.completed = true;
  result.blocked = true;
  result.collision_ended = true;
  return result;
}

auto ApplyMovementCommandCurve(const MovementState& previous,
                               const MovementCommand& command,
                               const MovementCurve& curve,
                               uint16_t dt_ms) -> MovementCommandStepResult {
  return ApplyMovementCommand(previous, command, curve, dt_ms,
                              DefaultMovementCommandResolver());
}

namespace {

void ApplyHorizontalIntent(MovementState& state, const InputFrame& input,
                           const MovementConfig& config, float dt) {
  const auto desired = DesiredMove(input);
  const auto horizontal_velocity = Horizontal(state.velocity);
  const auto target_velocity = desired * NonNegativeFinite(config.max_speed_mps);
  const float accel = desired.LengthSquared() > math::kEpsilon ? config.acceleration_mps2
                                                               : config.deceleration_mps2;
  const auto moved_horizontal =
      MoveTowards(horizontal_velocity, target_velocity, NonNegativeFinite(accel) * dt);
  state.velocity.x = moved_horizontal.x;
  state.velocity.z = moved_horizontal.z;
  if (desired.LengthSquared() > math::kEpsilon) state.direction = desired.Normalized();
}

[[nodiscard]] bool ApplyVerticalIntent(MovementStepResult& result, const MovementState& previous,
                                       const InputFrame& input, const MovementConfig& config,
                                       const CharacterQuery& query, float dt) {
  result.ground = query.GroundProbe(previous.position);
  const float ground_snap = NonNegativeFinite(config.ground_snap_distance_m);
  const float depenetration_budget = NonNegativeFinite(config.max_depenetration_m);
  const bool was_grounded =
      (previous.flags & kMovementFlagGrounded) != 0 &&
      IsWalkableGround(result.ground, config) &&
      previous.position.y >= result.ground.position.y - depenetration_budget &&
      previous.position.y <= result.ground.position.y + ground_snap;
  if (was_grounded && (input.buttons & kInputButtonJump) != 0) {
    result.state.velocity.y = NonNegativeFinite(config.jump_speed_mps);
    ClearGrounded(result.state);
    result.jumped = true;
  } else if (was_grounded) {
    result.state.velocity.y = 0.0f;
  } else {
    result.state.velocity.y -= NonNegativeFinite(config.gravity_mps2) * dt;
    result.state.velocity.y =
        std::max(result.state.velocity.y, -NonNegativeFinite(config.max_fall_speed_mps));
  }
  return was_grounded;
}

void ResolveCollisionAndSlide(MovementStepResult& result, const MovementConfig& config,
                              const CharacterQuery& query, bool was_grounded, float dt) {
  Capsule capsule;
  capsule.center = result.state.position;
  capsule.radius_m = NonNegativeFinite(config.capsule_radius_m);
  capsule.half_height_m = NonNegativeFinite(config.capsule_half_height_m);
  result.depenetrated = TryDepenetrate(&result.state, capsule, config, query);

  const auto displacement = result.state.velocity * dt;
  capsule.center = result.state.position;
  result.sweep = query.SweepCapsule({capsule, displacement});
  if (!result.sweep.hit) {
    result.state.position += displacement;
    return;
  }

  math::Vector3 stepped_position;
  if (was_grounded && !result.jumped &&
      TryStepUp(result.state.position, displacement, capsule, config, query,
                &stepped_position)) {
    result.state.position = stepped_position;
    result.sweep = {};
    result.stepped = true;
    return;
  }

  result.blocked = true;
  const float fraction = std::clamp(result.sweep.fraction, 0.0f, 1.0f);
  result.state.position += displacement * fraction;
  ClipVelocityAgainstSurface(&result.state, result.sweep.normal);

  const float remaining_fraction = 1.0f - fraction;
  const auto slide_displacement = result.state.velocity * dt * remaining_fraction;
  if (slide_displacement.LengthSquared() <= math::kEpsilon) return;

  capsule.center = result.state.position + SurfaceOffset(result.sweep.normal);
  const auto slide_hit = query.SweepCapsule({capsule, slide_displacement});
  if (slide_hit.hit) {
    const float slide_fraction = std::clamp(slide_hit.fraction, 0.0f, 1.0f);
    result.state.position += slide_displacement * slide_fraction;
    ClipVelocityAgainstSurface(&result.state, slide_hit.normal);
  } else {
    result.state.position += slide_displacement;
  }
}

void FinalizeGroundAndDirection(MovementStepResult& result, const MovementConfig& config,
                                const CharacterQuery& query) {
  result.ground = query.GroundProbe(result.state.position);
  if (result.jumped ||
      !TrySnapToGround(&result.state, result.ground, config, &result.snapped)) {
    ClearGrounded(result.state);
  }
  result.state.direction = SafeDirection(result.state.direction);
}

}  // namespace

auto Step(const MovementState& previous, const InputFrame& input, const MovementConfig& config,
          const CharacterQuery& query, uint32_t server_tick) -> MovementStepResult {
  MovementStepResult result;
  result.state = previous;
  result.state.last_processed_input_seq = input.seq;
  result.server_tick = server_tick;

  const float dt = NonNegativeFinite(config.fixed_dt_s);
  ApplyHorizontalIntent(result.state, input, config, dt);
  const bool was_grounded = ApplyVerticalIntent(result, previous, input, config, query, dt);
  ResolveCollisionAndSlide(result, config, query, was_grounded, dt);
  FinalizeGroundAndDirection(result, config, query);
  return result;
}

}  // namespace atlas::movement
