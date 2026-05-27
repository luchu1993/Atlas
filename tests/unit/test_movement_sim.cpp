#include <limits>

#include <gtest/gtest.h>

#include "movement_sim/movement_curve_store.h"
#include "movement_sim/movement_sim.h"

using atlas::math::Vector3;
using namespace atlas::movement;

namespace physics = atlas::physics;

namespace {

constexpr float kEpsilon = 1e-4f;

struct BlockingQuery final : CharacterQuery {
  auto GroundProbe(const Vector3& position) const -> GroundHit override {
    GroundHit hit;
    hit.hit = true;
    hit.position = {position.x, 0.0f, position.z};
    hit.distance_m = position.y;
    return hit;
  }

  auto SweepCapsule(const CapsuleCast&) const -> SweepHit override {
    SweepHit hit;
    hit.hit = true;
    hit.fraction = 0.25f;
    hit.normal = {0.0f, 0.0f, -1.0f};
    return hit;
  }

  auto OverlapCapsule(const Capsule&) const -> bool override { return false; }
};

struct BlockingPhysicsQuery final : physics::PhysicsQuery {
  auto GroundProbe(const physics::GroundProbeQuery& query) const -> physics::GroundHit override {
    physics::GroundHit hit;
    hit.hit = true;
    hit.position = {query.origin.x, 0.0f, query.origin.z};
    hit.distance_m = query.origin.y;
    return hit;
  }

  auto CastCapsule(const physics::CapsuleCastQuery&) const -> physics::ShapeCastHit override {
    physics::ShapeCastHit hit;
    hit.hit = true;
    hit.fraction = 0.25f;
    hit.normal = {0.0f, 0.0f, -1.0f};
    return hit;
  }

  auto OverlapCapsule(const physics::OverlapQuery&) const -> bool override { return false; }
};

struct DepenetratingPhysicsQuery final : physics::PhysicsQuery {
  explicit DepenetratingPhysicsQuery(Vector3 offset) : offset(offset) {}

  auto GroundProbe(const physics::GroundProbeQuery& query) const -> physics::GroundHit override {
    physics::GroundHit hit;
    hit.hit = true;
    hit.position = {query.origin.x, 0.0f, query.origin.z};
    hit.distance_m = query.origin.y;
    return hit;
  }

  auto CastCapsule(const physics::CapsuleCastQuery&) const -> physics::ShapeCastHit override {
    return {};
  }

  auto OverlapCapsule(const physics::OverlapQuery&) const -> bool override { return true; }

  auto DepenetrateCapsule(const physics::OverlapQuery&) const
      -> physics::DepenetrationHit override {
    physics::DepenetrationHit hit;
    hit.hit = true;
    hit.offset = offset;
    hit.depth_m = offset.Length();
    return hit;
  }

  Vector3 offset;
};

struct SlopedGroundQuery final : CharacterQuery {
  explicit SlopedGroundQuery(Vector3 normal) : normal(normal) {}

  auto GroundProbe(const Vector3& position) const -> GroundHit override {
    GroundHit hit;
    hit.hit = true;
    hit.position = {position.x, 0.0f, position.z};
    hit.normal = normal;
    hit.distance_m = position.y;
    return hit;
  }

  auto SweepCapsule(const CapsuleCast&) const -> SweepHit override { return {}; }
  auto OverlapCapsule(const Capsule&) const -> bool override { return false; }

  Vector3 normal;
};

struct StepUpQuery final : CharacterQuery {
  auto GroundProbe(const Vector3& position) const -> GroundHit override {
    GroundHit hit;
    hit.hit = true;
    hit.position = {position.x, position.z >= 0.25f ? 0.25f : 0.0f, position.z};
    hit.distance_m = position.y - hit.position.y;
    return hit;
  }

  auto SweepCapsule(const CapsuleCast& cast) const -> SweepHit override {
    if (cast.capsule.center.y >= 0.3f) return {};
    SweepHit hit;
    hit.hit = true;
    hit.fraction = 0.2f;
    hit.normal = {0.0f, 0.0f, -1.0f};
    return hit;
  }

  auto OverlapCapsule(const Capsule&) const -> bool override { return false; }
};

struct DepenetratingStepUpQuery final : CharacterQuery {
  auto GroundProbe(const Vector3& position) const -> GroundHit override {
    GroundHit hit;
    hit.hit = true;
    hit.position = {position.x, position.z >= 0.25f ? 0.25f : 0.0f, position.z};
    hit.distance_m = position.y - hit.position.y;
    return hit;
  }

  auto SweepCapsule(const CapsuleCast& cast) const -> SweepHit override {
    if (cast.capsule.center.y >= 0.3f) return {};
    SweepHit hit;
    hit.hit = true;
    hit.fraction = 0.2f;
    hit.normal = {0.0f, 0.0f, -1.0f};
    return hit;
  }

  auto OverlapCapsule(const Capsule&) const -> bool override { return false; }

  auto DepenetrateCapsule(const Capsule&) const -> DepenetrationHit override {
    DepenetrationHit hit;
    hit.hit = true;
    hit.offset = {0.2f, 0.0f, 0.0f};
    hit.normal = {1.0f, 0.0f, 0.0f};
    hit.depth_m = 0.2f;
    return hit;
  }
};

struct DropGroundQuery final : CharacterQuery {
  explicit DropGroundQuery(float lower_ground_y) : lower_ground_y(lower_ground_y) {}

  auto GroundProbe(const Vector3& position) const -> GroundHit override {
    GroundHit hit;
    hit.hit = true;
    hit.position = {position.x, position.z >= 0.25f ? lower_ground_y : 0.0f, position.z};
    hit.distance_m = position.y - hit.position.y;
    return hit;
  }

  auto SweepCapsule(const CapsuleCast&) const -> SweepHit override { return {}; }
  auto OverlapCapsule(const Capsule&) const -> bool override { return false; }

  float lower_ground_y;
};

struct DepenetratingQuery final : CharacterQuery {
  explicit DepenetratingQuery(Vector3 offset) : offset(offset) {}

  auto GroundProbe(const Vector3& position) const -> GroundHit override {
    GroundHit hit;
    hit.hit = true;
    hit.position = {position.x, 0.0f, position.z};
    hit.distance_m = position.y;
    return hit;
  }

  auto SweepCapsule(const CapsuleCast&) const -> SweepHit override { return {}; }
  auto OverlapCapsule(const Capsule&) const -> bool override { return true; }

  auto DepenetrateCapsule(const Capsule&) const -> DepenetrationHit override {
    DepenetrationHit hit;
    hit.hit = true;
    hit.offset = offset;
    hit.depth_m = offset.Length();
    return hit;
  }

  Vector3 offset;
};

struct FixedCommandSampler final : MovementCommandSampler {
  auto Supports(MovementCommandType type) const -> bool override {
    return type == MovementCommandType::kDash;
  }

  auto Step(const MovementState& previous, const MovementCommand& command,
            const MovementCurve&, uint16_t) const -> MovementCommandStepResult override {
    MovementCommandStepResult result;
    result.state = previous;
    result.command = command;
    result.state.position = {42.0f, 0.0f, 0.0f};
    result.completed = true;
    return result;
  }
};

struct FixedCommandResolver final : MovementCommandResolver {
  auto Find(MovementCommandType type) const -> const MovementCommandSampler* override {
    return sampler.Supports(type) ? &sampler : nullptr;
  }

  FixedCommandSampler sampler;
};

struct FixedCommandPolicy final : MovementCommandPolicy {
  auto SuppressesInput(const MovementCommand&) const -> bool override { return false; }
  auto AllowsTurnInput(const MovementCommand&) const -> bool override { return false; }
  auto ChecksCollision(const MovementCommand&) const -> bool override { return false; }
  auto EndReasonFor(const MovementCommandStepResult&) const
      -> MovementCommandEndReason override {
    return MovementCommandEndReason::kCancelled;
  }
};

auto DefaultConfig() -> MovementConfig {
  MovementConfig config;
  config.fixed_dt_s = 0.1f;
  config.max_speed_mps = 5.0f;
  config.acceleration_mps2 = 100.0f;
  config.deceleration_mps2 = 100.0f;
  config.ground_snap_distance_m = 0.2f;
  return config;
}

auto LinearCurve(uint16_t id = 7) -> MovementCurve {
  MovementCurve curve;
  curve.id = id;
  curve.sample_count = 3;
  curve.samples[0] = 0.0f;
  curve.samples[1] = 0.5f;
  curve.samples[2] = 1.0f;
  return curve;
}

}  // namespace

TEST(MovementSim, ForwardInputAdvancesOnYawZero) {
  FlatGroundQuery query;
  MovementState state;
  InputFrame input;
  input.seq = 7;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 11);

  EXPECT_NEAR(result.state.position.x, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.z, 5.0f, kEpsilon);
  EXPECT_EQ(result.state.last_processed_input_seq, 7u);
  EXPECT_EQ(result.server_tick, 11u);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_FALSE(result.snapped);
}

TEST(MovementSim, DiagonalInputDoesNotExceedMaxSpeed) {
  FlatGroundQuery query;
  MovementState state;
  InputFrame input;
  input.move_x = 127;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);
  auto horizontal_speed =
      Vector3{result.state.velocity.x, 0.0f, result.state.velocity.z}.Length();

  EXPECT_NEAR(horizontal_speed, 5.0f, kEpsilon);
}

TEST(MovementSim, NoInputDeceleratesHorizontalVelocity) {
  FlatGroundQuery query;
  MovementState state;
  state.velocity = {5.0f, 0.0f, 0.0f};
  InputFrame input;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_NEAR(result.state.velocity.x, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.z, 0.0f, kEpsilon);
}

TEST(MovementSim, JumpClearsGroundedAndAppliesGravityNextTick) {
  FlatGroundQuery query;
  auto config = DefaultConfig();
  config.jump_speed_mps = 6.0f;
  config.gravity_mps2 = 10.0f;

  MovementState state;
  InputFrame jump;
  jump.buttons = kInputButtonJump;

  auto jumped = Step(state, jump, config, query, 1);
  EXPECT_TRUE(jumped.jumped);
  EXPECT_FALSE((jumped.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_NEAR(jumped.state.velocity.y, 6.0f, kEpsilon);

  InputFrame fall;
  auto falling = Step(jumped.state, fall, config, query, 2);
  EXPECT_NEAR(falling.state.velocity.y, 5.0f, kEpsilon);
  EXPECT_GT(falling.state.position.y, 0.0f);
}

TEST(MovementSim, FallingVelocityClampsToConfiguredLimit) {
  FlatGroundQuery query;
  auto config = DefaultConfig();
  config.gravity_mps2 = 20.0f;
  config.max_fall_speed_mps = 6.0f;

  MovementState state;
  state.flags = 0;
  state.position.y = 10.0f;
  state.velocity.y = -5.0f;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_NEAR(result.state.velocity.y, -6.0f, kEpsilon);
}

TEST(MovementSim, FlatGroundQueryStopsLargeFallOnGround) {
  FlatGroundQuery query;
  MovementState state;
  state.flags = 0;
  state.position.y = 0.2f;
  state.velocity.y = -10.0f;
  InputFrame input;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.blocked);
  EXPECT_NEAR(result.sweep.fraction, 0.16f, kEpsilon);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.y, 0.0f, kEpsilon);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, SweepHitConsumesPartialDisplacementAndSlidesVelocity) {
  BlockingQuery query;
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.blocked);
  EXPECT_NEAR(result.state.position.z, 0.125f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.z, 0.0f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryUsesPhysicsGroundProbe) {
  physics::FlatPhysicsQuery physics_query;
  PhysicsCharacterQuery query(physics_query);
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, PhysicsCharacterQueryConvertsCapsuleSweep) {
  BlockingPhysicsQuery physics_query;
  PhysicsCharacterQuery query(physics_query);
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.blocked);
  EXPECT_NEAR(result.state.position.z, 0.125f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.z, 0.0f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryConvertsDepenetration) {
  DepenetratingPhysicsQuery physics_query({0.2f, 0.0f, 0.0f});
  PhysicsCharacterQuery query(physics_query);
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_NEAR(result.state.position.x, 0.2f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryStepsOnStaticBox) {
  physics::StaticPhysicsQuery physics_query;
  physics_query.AddBox(
      physics::StaticBox{{-0.5f, 0.0f, 0.5f}, {0.5f, 0.25f, 0.8f}, 0});
  PhysicsCharacterQuery query(physics_query);
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.stepped);
  EXPECT_FALSE(result.blocked);
  EXPECT_NEAR(result.state.position.y, 0.25f, kEpsilon);
  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQuerySlidesAlongStaticBoxWall) {
  physics::StaticPhysicsQuery physics_query;
  physics_query.AddBox(
      physics::StaticBox{{-1.0f, 0.0f, 0.55f}, {1.0f, 2.0f, 1.0f}, 0});
  PhysicsCharacterQuery query(physics_query);
  MovementState state;
  InputFrame input;
  input.move_x = 127;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.blocked);
  EXPECT_FALSE(result.stepped);
  EXPECT_GT(result.state.position.x, 0.3f);
  EXPECT_NEAR(result.state.position.z, 0.2f, 0.001f);
  EXPECT_GT(result.state.velocity.x, 0.0f);
  EXPECT_NEAR(result.state.velocity.z, 0.0f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryGroundProbeUsesConfiguredRadius) {
  physics::StaticPhysicsQuery physics_query;
  physics_query.AddBox(
      physics::StaticBox{{0.3f, 0.0f, -0.2f}, {0.6f, 0.25f, 0.2f}, 1});
  PhysicsCharacterQuery point_query(physics_query, 2.0f, physics::LayerMask{1u << 1});
  PhysicsCharacterQuery capsule_query(
      physics_query, 2.0f, physics::LayerMask{1u << 1}, 0.35f);

  EXPECT_FALSE(point_query.GroundProbe({0.0f, 1.0f, 0.0f}).hit);

  auto hit = capsule_query.GroundProbe({0.0f, 1.0f, 0.0f});
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 0.25f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryStopsLargeFallOnStaticGround) {
  physics::StaticPhysicsQuery physics_query;
  PhysicsCharacterQuery query(physics_query);
  auto config = DefaultConfig();
  config.gravity_mps2 = 0.0f;
  MovementState state;
  state.flags = 0;
  state.position.y = 0.2f;
  state.velocity.y = -5.0f;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_TRUE(result.blocked);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.y, 0.0f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryDepenetratesStaticGround) {
  physics::StaticPhysicsQuery physics_query;
  PhysicsCharacterQuery query(physics_query);
  auto config = DefaultConfig();
  config.gravity_mps2 = 0.0f;
  MovementState state;
  state.flags = 0;
  state.position.y = -0.05f;
  state.velocity.y = -3.0f;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.y, 0.0f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryKeepsWalkableStaticPlaneGrounded) {
  physics::StaticPhysicsQuery physics_query;
  physics_query.AddPlane(physics::StaticPlane{{0.0f, 0.0f, 0.0f},
                                              {0.0f, 4.0f, 3.0f}, 1});
  PhysicsCharacterQuery query(physics_query, 2.0f, physics::LayerMask{1u << 1});
  auto config = DefaultConfig();
  config.max_walkable_slope_degrees = 45.0f;
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_NEAR(result.ground.normal.y, 0.8f, kEpsilon);
}

TEST(MovementSim, PhysicsCharacterQueryRejectsSteepStaticPlane) {
  physics::StaticPhysicsQuery physics_query;
  physics_query.AddPlane(physics::StaticPlane{{0.0f, 0.0f, 0.0f},
                                              {0.0f, 0.5f, 0.8660254f}, 1});
  PhysicsCharacterQuery query(physics_query, 2.0f, physics::LayerMask{1u << 1});
  auto config = DefaultConfig();
  config.max_walkable_slope_degrees = 45.0f;
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_FALSE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_LT(result.state.velocity.y, 0.0f);
}

TEST(MovementSim, WalkableSlopeKeepsGrounded) {
  SlopedGroundQuery query({0.0f, 0.8f, 0.6f});
  auto config = DefaultConfig();
  config.max_walkable_slope_degrees = 45.0f;
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, SteepSlopeClearsGrounded) {
  SlopedGroundQuery query({0.0f, 0.5f, 0.8660254f});
  auto config = DefaultConfig();
  config.max_walkable_slope_degrees = 45.0f;
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_FALSE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_LT(result.state.velocity.y, 0.0f);
}

TEST(MovementSim, StepUpClimbsSmallObstacleAfterSweepHit) {
  StepUpQuery query;
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.stepped);
  EXPECT_FALSE(result.blocked);
  EXPECT_NEAR(result.state.position.y, 0.25f, kEpsilon);
  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, StepUpPreservesDepenetratedStartPosition) {
  DepenetratingStepUpQuery query;
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_TRUE(result.stepped);
  EXPECT_NEAR(result.state.position.x, 0.2f, kEpsilon);
  EXPECT_NEAR(result.state.position.y, 0.25f, kEpsilon);
  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
}

TEST(MovementSim, JumpInputDoesNotStepUpAfterSweepHit) {
  StepUpQuery query;
  MovementState state;
  InputFrame input;
  input.move_z = 127;
  input.buttons = kInputButtonJump;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.jumped);
  EXPECT_TRUE(result.blocked);
  EXPECT_FALSE(result.stepped);
  EXPECT_GT(result.state.velocity.y, 0.0f);
}

TEST(MovementSim, JumpInputDoesNotSnapAfterLowCeilingHit) {
  physics::StaticPhysicsQuery physics_query;
  physics_query.AddBox(
      physics::StaticBox{{-1.0f, 1.95f, -1.0f}, {1.0f, 2.1f, 1.0f}, 0});
  PhysicsCharacterQuery query(physics_query);
  MovementState state;
  InputFrame input;
  input.buttons = kInputButtonJump;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.jumped);
  EXPECT_TRUE(result.blocked);
  EXPECT_FALSE((result.state.flags & kMovementFlagGrounded) != 0);
  EXPECT_GT(result.state.position.y, 0.1f);
  EXPECT_NEAR(result.state.velocity.y, 0.0f, kEpsilon);
}

TEST(MovementSim, JumpInputDoesNotBypassDepenetrationBudget) {
  FlatGroundQuery query;
  auto config = DefaultConfig();
  config.gravity_mps2 = 0.0f;
  config.max_depenetration_m = 0.25f;
  MovementState state;
  state.position.y = -1.0f;
  state.flags = kMovementFlagGrounded;
  InputFrame input;
  input.buttons = kInputButtonJump;

  auto result = Step(state, input, config, query, 1);

  EXPECT_FALSE(result.jumped);
  EXPECT_TRUE(result.depenetrated);
  EXPECT_NEAR(result.state.position.y, -0.75f, kEpsilon);
  EXPECT_FALSE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, StepUpDisabledKeepsSweepBlocked) {
  StepUpQuery query;
  auto config = DefaultConfig();
  config.step_height_m = 0.0f;
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, config, query, 1);

  EXPECT_FALSE(result.stepped);
  EXPECT_TRUE(result.blocked);
  EXPECT_NEAR(result.state.position.z, 0.1f, kEpsilon);
}

TEST(MovementSim, SnapToGroundFollowsSmallDrop) {
  DropGroundQuery query(-0.15f);
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.snapped);
  EXPECT_NEAR(result.state.position.y, -0.15f, kEpsilon);
  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
  EXPECT_TRUE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, SnapToGroundIgnoresLargeDrop) {
  DropGroundQuery query(-0.5f);
  MovementState state;
  InputFrame input;
  input.move_z = 127;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_FALSE(result.snapped);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(result.state.position.z, 0.5f, kEpsilon);
  EXPECT_FALSE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, DepenetrationAppliesQueryOffset) {
  DepenetratingQuery query({0.2f, 0.0f, 0.0f});
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_NEAR(result.state.position.x, 0.2f, kEpsilon);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
}

TEST(MovementSim, DepenetrationClampsToConfiguredBudget) {
  DepenetratingQuery query({1.0f, 0.0f, 0.0f});
  auto config = DefaultConfig();
  config.max_depenetration_m = 0.25f;
  MovementState state;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_NEAR(result.state.position.x, 0.25f, kEpsilon);
}

TEST(MovementSim, SnapToGroundDoesNotBypassDepenetrationBudget) {
  FlatGroundQuery query;
  auto config = DefaultConfig();
  config.gravity_mps2 = 0.0f;
  config.max_depenetration_m = 0.25f;
  MovementState state;
  state.position.y = -1.0f;
  state.flags = 0;
  InputFrame input;

  auto result = Step(state, input, config, query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_FALSE(result.snapped);
  EXPECT_NEAR(result.state.position.y, -0.75f, kEpsilon);
  EXPECT_FALSE((result.state.flags & kMovementFlagGrounded) != 0);
}

TEST(MovementSim, FlatGroundQueryDepenetratesGround) {
  FlatGroundQuery query;
  Capsule capsule;
  capsule.center = {0.0f, -0.05f, 0.0f};
  capsule.radius_m = 0.35f;
  capsule.half_height_m = 0.9f;

  EXPECT_TRUE(query.OverlapCapsule(capsule));
  auto hit = query.DepenetrateCapsule(capsule);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.offset.y, 0.05f, kEpsilon);
  EXPECT_NEAR(hit.depth_m, 0.05f, kEpsilon);

  MovementState state;
  state.position.y = -0.05f;
  state.flags = 0;
  InputFrame input;

  auto result = Step(state, input, DefaultConfig(), query, 1);

  EXPECT_TRUE(result.depenetrated);
  EXPECT_NEAR(result.state.position.y, 0.0f, kEpsilon);
}

TEST(MovementSim, FlatPhysicsQueryRespectsProbeDistanceAndLayerMask) {
  physics::FlatPhysicsQuery query;
  physics::GroundProbeQuery probe;
  probe.origin.y = 1.0f;
  probe.max_distance_m = 0.5f;
  EXPECT_FALSE(query.GroundProbe(probe).hit);

  probe.max_distance_m = 1.0f;
  EXPECT_TRUE(query.GroundProbe(probe).hit);

  probe.filter.mask.bits = 0;
  EXPECT_FALSE(query.GroundProbe(probe).hit);
}

TEST(MovementSim, FlatPhysicsQueryGroundProbeRejectsNonFiniteOrigin) {
  physics::FlatPhysicsQuery query;
  physics::GroundProbeQuery probe;
  probe.origin = {std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f};
  probe.max_distance_m = 2.0f;

  EXPECT_FALSE(query.GroundProbe(probe).hit);
}

TEST(MovementSim, FlatPhysicsQueryRejectsNonFiniteCapsule) {
  physics::FlatPhysicsQuery query;
  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 0.2f, 0.0f};
  cast.capsule.radius_m = std::numeric_limits<float>::quiet_NaN();
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, -0.5f, 0.0f};

  EXPECT_FALSE(query.CastCapsule(cast).hit);

  physics::OverlapQuery overlap;
  overlap.capsule = cast.capsule;
  overlap.capsule.center.y = -0.05f;
  EXPECT_FALSE(query.OverlapCapsule(overlap));
  EXPECT_FALSE(query.DepenetrateCapsule(overlap).hit);
}

TEST(MovementSim, FlatPhysicsQueryRaycastUsesGroundAndLayerMask) {
  physics::FlatPhysicsQuery query;
  physics::RaycastQuery ray;
  ray.origin = {0.0f, 1.0f, 0.0f};
  ray.direction = {0.0f, -2.0f, 0.0f};
  ray.max_distance_m = 2.0f;

  auto hit = query.Raycast(ray);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.distance_m, 1.0f, kEpsilon);
  EXPECT_NEAR(hit.fraction, 0.5f, kEpsilon);
  EXPECT_NEAR(hit.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 1.0f, kEpsilon);

  ray.filter.mask.bits = 0;
  EXPECT_FALSE(query.Raycast(ray).hit);
}

TEST(MovementSim, FlatPhysicsQueryCapsuleCastHitsGroundAndLayerMask) {
  physics::FlatPhysicsQuery query;
  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 0.2f, 0.0f};
  cast.capsule.radius_m = 0.35f;
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, -0.5f, 0.0f};

  auto hit = query.CastCapsule(cast);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.fraction, 0.4f, kEpsilon);
  EXPECT_NEAR(hit.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 1.0f, kEpsilon);

  cast.filter.mask.bits = 0;
  EXPECT_FALSE(query.CastCapsule(cast).hit);
}

TEST(MovementSim, FlatPhysicsQueryDepenetratesGroundAndLayerMask) {
  physics::FlatPhysicsQuery query;
  physics::OverlapQuery overlap;
  overlap.capsule.center = {0.0f, -0.05f, 0.0f};
  overlap.capsule.radius_m = 0.35f;
  overlap.capsule.half_height_m = 0.9f;

  EXPECT_TRUE(query.OverlapCapsule(overlap));
  auto hit = query.DepenetrateCapsule(overlap);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 0u);
  EXPECT_NEAR(hit.offset.y, 0.05f, kEpsilon);
  EXPECT_NEAR(hit.depth_m, 0.05f, kEpsilon);

  overlap.filter.mask.bits = 0;
  EXPECT_FALSE(query.OverlapCapsule(overlap));
  EXPECT_FALSE(query.DepenetrateCapsule(overlap).hit);
}

TEST(MovementSim, StaticPhysicsQueryGroundProbeUsesPlaneNormalAndLayerMask) {
  physics::StaticPhysicsQuery query;
  query.AddPlane(physics::StaticPlane{{0.0f, 0.0f, 0.0f},
                                      {0.0f, 4.0f, 3.0f}, 1});

  physics::GroundProbeQuery probe;
  probe.origin = {0.0f, 1.0f, 0.0f};
  probe.max_distance_m = 2.0f;
  probe.filter.mask.bits = 1u << 1;

  auto hit = query.GroundProbe(probe);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 0.8f, kEpsilon);

  probe.filter.mask.bits = 1u << 2;
  EXPECT_FALSE(query.GroundProbe(probe).hit);
}

TEST(MovementSim, StaticPhysicsQueryPrefersExplicitPlaneOverFlatAtSameHeight) {
  physics::StaticPhysicsQuery query;
  query.AddPlane(physics::StaticPlane{{0.0f, 0.0f, 0.0f},
                                      {0.0f, 0.5f, 0.8660254f}, 0});

  physics::GroundProbeQuery probe;
  probe.origin = {0.0f, 1.0f, 0.0f};
  probe.max_distance_m = 2.0f;

  auto hit = query.GroundProbe(probe);

  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 0.0f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 0.5f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryPrefersExplicitBoxOverFlatAtSameHeight) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.0f, 0.5f}, 1});

  physics::GroundProbeQuery probe;
  probe.origin = {0.0f, 1.0f, 0.0f};
  probe.max_distance_m = 2.0f;
  probe.filter.mask.bits = (1u << 0) | (1u << 1);

  auto hit = query.GroundProbe(probe);

  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.position.y, 0.0f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryRaycastHitsNearestBoxAndLayerMask) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{-1.0f, 0.0f, 0.5f}, {1.0f, 1.0f, 1.0f}, 1});

  physics::RaycastQuery ray;
  ray.origin = {0.0f, 0.5f, 0.0f};
  ray.direction = {0.0f, 0.0f, 2.0f};
  ray.max_distance_m = 2.0f;
  ray.filter.mask.bits = 1u << 1;

  auto hit = query.Raycast(ray);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.distance_m, 0.5f, kEpsilon);
  EXPECT_NEAR(hit.position.z, 0.5f, kEpsilon);
  EXPECT_NEAR(hit.normal.z, -1.0f, kEpsilon);

  ray.filter.mask.bits = 1u << 2;
  EXPECT_FALSE(query.Raycast(ray).hit);
}

TEST(MovementSim, StaticPhysicsQueryRaycastHitsPlane) {
  physics::StaticPhysicsQuery query;
  query.AddPlane(physics::StaticPlane{{0.0f, 0.5f, 0.0f},
                                      {0.0f, 4.0f, 3.0f}, 1});

  physics::RaycastQuery ray;
  ray.origin = {0.0f, 2.0f, 0.0f};
  ray.direction = {0.0f, -1.0f, 0.0f};
  ray.max_distance_m = 4.0f;
  ray.filter.mask.bits = 1u << 1;

  auto hit = query.Raycast(ray);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.distance_m, 1.5f, kEpsilon);
  EXPECT_NEAR(hit.position.y, 0.5f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 0.8f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryCapsuleCastHitsPlane) {
  physics::StaticPhysicsQuery query;
  query.AddPlane(physics::StaticPlane{{0.0f, 0.5f, 0.0f},
                                      {0.0f, 4.0f, 3.0f}, 1});

  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 2.0f, 0.0f};
  cast.capsule.radius_m = 0.35f;
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, -2.0f, 0.0f};
  cast.filter.mask.bits = 1u << 1;

  auto hit = query.CastCapsule(cast);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.fraction, 0.75f, kEpsilon);
  EXPECT_NEAR(hit.position.y, 0.5f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 0.8f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryCapsuleCastHitsSteepPlaneHorizontally) {
  physics::StaticPhysicsQuery query;
  query.AddPlane(physics::StaticPlane{{0.0f, 0.0f, 0.25f},
                                      {0.0f, 0.5f, -0.8660254f}, 1});

  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 0.0f, 0.0f};
  cast.capsule.radius_m = 0.35f;
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, 0.0f, 1.0f};
  cast.filter.mask.bits = 1u << 1;

  auto hit = query.CastCapsule(cast);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.fraction, 0.25f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 0.5f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryDepenetratesPlaneAndLayerMask) {
  physics::StaticPhysicsQuery query;
  query.AddPlane(physics::StaticPlane{{0.0f, 0.5f, 0.0f}, {0.0f, 2.0f, 0.0f}, 1});

  physics::OverlapQuery overlap;
  overlap.capsule.center = {0.0f, 0.45f, 0.0f};
  overlap.capsule.radius_m = 0.35f;
  overlap.capsule.half_height_m = 0.9f;
  overlap.filter.mask.bits = 1u << 1;

  EXPECT_TRUE(query.OverlapCapsule(overlap));
  auto hit = query.DepenetrateCapsule(overlap);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.offset.y, 0.05f, kEpsilon);
  EXPECT_NEAR(hit.normal.y, 1.0f, kEpsilon);

  overlap.filter.mask.bits = 1u << 2;
  EXPECT_FALSE(query.OverlapCapsule(overlap));
  EXPECT_FALSE(query.DepenetrateCapsule(overlap).hit);
}

TEST(MovementSim, StaticPhysicsQueryGroundProbeUsesBoxTopAndLayerMask) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{-1.0f, 0.0f, 1.0f}, {1.0f, 0.5f, 2.0f}, 1});

  physics::GroundProbeQuery probe;
  probe.origin = {0.0f, 1.0f, 1.5f};
  probe.max_distance_m = 2.0f;
  probe.filter.mask.bits = 1u << 1;

  auto hit = query.GroundProbe(probe);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.layer, 1u);
  EXPECT_NEAR(hit.position.y, 0.5f, kEpsilon);

  probe.filter.mask.bits = 1u << 2;
  EXPECT_FALSE(query.GroundProbe(probe).hit);
}

TEST(MovementSim, StaticPhysicsQueryGroundProbeRejectsBoxCornerOutsideRadius) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{-0.5f, 0.0f, -0.5f}, {0.5f, 0.25f, 0.5f}, 1});

  physics::GroundProbeQuery probe;
  probe.origin = {0.84f, 1.0f, 0.84f};
  probe.max_distance_m = 2.0f;
  probe.radius_m = 0.35f;
  probe.filter.mask.bits = 1u << 1;

  EXPECT_FALSE(query.GroundProbe(probe).hit);

  probe.origin = {0.84f, 1.0f, 0.5f};
  auto hit = query.GroundProbe(probe);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 0.25f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryCapsuleCastHitsExpandedBox) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{-1.0f, 0.0f, 0.5f}, {1.0f, 1.0f, 0.8f}, 0});

  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 0.0f, 0.0f};
  cast.capsule.radius_m = 0.35f;
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, 0.0f, 1.0f};

  auto hit = query.CastCapsule(cast);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.fraction, 0.15f, kEpsilon);
  EXPECT_NEAR(hit.normal.z, -1.0f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryCapsuleCastAllowsParallelBoxContactMotion) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{0.35f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}, 1});

  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 0.0f, 0.0f};
  cast.capsule.radius_m = 0.35f;
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, 0.0f, 0.5f};
  cast.filter.mask.bits = 1u << 1;

  EXPECT_FALSE(query.CastCapsule(cast).hit);

  cast.displacement = {0.1f, 0.0f, 0.0f};
  auto hit = query.CastCapsule(cast);
  EXPECT_TRUE(hit.hit);
  EXPECT_NEAR(hit.fraction, 0.0f, kEpsilon);
  EXPECT_NEAR(hit.normal.x, -1.0f, kEpsilon);
}

TEST(MovementSim, StaticPhysicsQueryRejectsNonFiniteCapsule) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{0.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}, 0});

  physics::CapsuleCastQuery cast;
  cast.capsule.center = {0.0f, 0.0f, 0.0f};
  cast.capsule.radius_m = std::numeric_limits<float>::quiet_NaN();
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {0.0f, 0.0f, 1.0f};

  EXPECT_FALSE(query.CastCapsule(cast).hit);

  physics::OverlapQuery overlap;
  overlap.capsule = cast.capsule;
  overlap.capsule.center = {0.2f, 0.0f, 0.0f};
  EXPECT_FALSE(query.OverlapCapsule(overlap));
  EXPECT_FALSE(query.DepenetrateCapsule(overlap).hit);
}

TEST(MovementSim, StaticPhysicsQueryDepenetratesOverlappingCapsule) {
  physics::StaticPhysicsQuery query;
  query.AddBox(physics::StaticBox{{0.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}, 0});

  physics::OverlapQuery overlap;
  overlap.capsule.center = {0.2f, 0.0f, 0.0f};
  overlap.capsule.radius_m = 0.35f;
  overlap.capsule.half_height_m = 0.9f;

  EXPECT_TRUE(query.OverlapCapsule(overlap));
  auto hit = query.DepenetrateCapsule(overlap);
  EXPECT_TRUE(hit.hit);
  EXPECT_LT(hit.offset.x, 0.0f);
  EXPECT_NEAR(hit.depth_m, 0.55f, kEpsilon);
}

TEST(MovementSim, FiniteStateRejectsNaN) {
  MovementState state;
  state.position.x = std::numeric_limits<float>::quiet_NaN();

  EXPECT_FALSE(IsFinite(state));
}

TEST(MovementSim, StateLimitsRejectUnsafeVelocityAndPosition) {
  auto config = DefaultConfig();
  MovementState state;

  EXPECT_TRUE(IsStateWithinLimits(state, config));

  state.velocity.x = config.max_speed_mps + 1.0f;
  EXPECT_FALSE(IsStateWithinLimits(state, config));

  state.velocity = {};
  state.velocity.y = -(config.max_fall_speed_mps + 1.0f);
  EXPECT_FALSE(IsStateWithinLimits(state, config));

  state.velocity = {};
  state.position.x = config.max_position_abs_m + 1.0f;
  EXPECT_FALSE(IsStateWithinLimits(state, config));
}

TEST(MovementSim, HorizontalAccelerationLimitUsesConfiguredTick) {
  auto config = DefaultConfig();
  MovementState previous;
  MovementState current;

  current.velocity.x = config.acceleration_mps2 * config.fixed_dt_s;
  EXPECT_TRUE(IsHorizontalAccelerationWithinLimits(previous, current, config));

  current.velocity.x += 1.0f;
  EXPECT_FALSE(IsHorizontalAccelerationWithinLimits(previous, current, config));
}

TEST(MovementSim, InputFrameRejectsInvalidClientDt) {
  InputFrame input;
  input.client_dt_ms = kMinInputDtMs;
  EXPECT_TRUE(IsInputFrameValid(input));
  input.client_dt_ms = kMaxInputDtMs;
  EXPECT_TRUE(IsInputFrameValid(input));
  input.client_dt_ms = 0;
  EXPECT_FALSE(IsInputFrameValid(input));
  input.client_dt_ms = static_cast<uint16_t>(kMaxInputDtMs + 1);
  EXPECT_FALSE(IsInputFrameValid(input));
}

TEST(MovementSim, InputFacingDirectionDecodesYaw) {
  InputFrame input;
  input.view_yaw = 0;
  auto direction = InputFacingDirection(input);
  EXPECT_NEAR(direction.x, 0.0f, kEpsilon);
  EXPECT_NEAR(direction.z, 1.0f, kEpsilon);

  input.view_yaw = 16384;
  direction = InputFacingDirection(input);
  EXPECT_NEAR(direction.x, 1.0f, 0.001f);
  EXPECT_NEAR(direction.z, 0.0f, 0.001f);
}

TEST(MovementSim, InputSequenceNewerHandlesWrap) {
  EXPECT_TRUE(IsInputSequenceNewer(2, 1));
  EXPECT_FALSE(IsInputSequenceNewer(1, 1));
  EXPECT_FALSE(IsInputSequenceNewer(1, 2));
  EXPECT_TRUE(IsInputSequenceNewer(0, uint32_t{0xFFFFFFFFu}));
  EXPECT_FALSE(IsInputSequenceNewer(uint32_t{0xFFFFFFFFu}, 0));
  EXPECT_EQ(InputSequenceDelta(3, 1), 2u);
}

TEST(MovementSim, CorrectionTierUsesSharedThresholds) {
  EXPECT_EQ(ClassifyCorrection(0.29f), CorrectionTier::kNone);
  EXPECT_EQ(ClassifyCorrection(kCorrectionTier1DistanceM), CorrectionTier::kTier1);
  EXPECT_EQ(ClassifyCorrection(kCorrectionTier2DistanceM), CorrectionTier::kTier2);
  EXPECT_EQ(ClassifyCorrection(kCorrectionSnapDistanceM), CorrectionTier::kSnap);
  EXPECT_EQ(CorrectionFlagForTier(CorrectionTier::kTier1), kCorrectionFlagTier1);
  EXPECT_EQ(CorrectionFlagForTier(CorrectionTier::kTier2), kCorrectionFlagTier2);
  EXPECT_EQ(CorrectionFlagForTier(CorrectionTier::kSnap), kCorrectionFlagSnap);
}

TEST(MovementSim, CorrectionFlagsValidAcceptsSingleTier) {
  EXPECT_TRUE(IsCorrectionFlagsValid(0));
  EXPECT_TRUE(IsCorrectionFlagsValid(kCorrectionFlagTier1));
  EXPECT_TRUE(IsCorrectionFlagsValid(kCorrectionFlagTier2));
  EXPECT_TRUE(IsCorrectionFlagsValid(kCorrectionFlagSnap));
  EXPECT_FALSE(IsCorrectionFlagsValid(kCorrectionFlagTier1 | kCorrectionFlagTier2));
  EXPECT_FALSE(IsCorrectionFlagsValid(kCorrectionFlagTier2 | kCorrectionFlagSnap));
  EXPECT_FALSE(IsCorrectionFlagsValid(static_cast<uint16_t>(1u << 5)));
  EXPECT_FALSE(IsCorrectionFlagsValid(static_cast<uint16_t>(0xFFFFu)));
}

TEST(MovementSim, MovementCurveSamplesLinearlyAndClamps) {
  auto curve = LinearCurve();

  EXPECT_TRUE(IsMovementCurveValid(curve));
  EXPECT_FLOAT_EQ(SampleMovementCurve(curve, -1.0f), 0.0f);
  EXPECT_FLOAT_EQ(SampleMovementCurve(curve, 0.25f), 0.25f);
  EXPECT_FLOAT_EQ(SampleMovementCurve(curve, 0.75f), 0.75f);
  EXPECT_FLOAT_EQ(SampleMovementCurve(curve, 2.0f), 1.0f);
}

TEST(MovementSim, MovementCommandCurveAdvancesPositionAndVelocity) {
  MovementState previous;
  MovementCommand command;
  command.command_id = 44;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;

  auto result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 500);

  EXPECT_TRUE(result.active);
  EXPECT_FALSE(result.completed);
  EXPECT_EQ(result.command.elapsed_ms, 500u);
  EXPECT_NEAR(result.state.position.x, 5.0f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.x, 10.0f, kEpsilon);
  EXPECT_NEAR(result.state.direction.x, 1.0f, kEpsilon);
}

TEST(MovementSim, MovementCommandUsesInjectedSampler) {
  MovementState previous;
  MovementCommand command;
  command.command_id = 48;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;
  FixedCommandResolver resolver;

  auto result = ApplyMovementCommand(previous, command, LinearCurve(), 500, resolver);

  EXPECT_TRUE(result.completed);
  EXPECT_NEAR(result.state.position.x, 42.0f, kEpsilon);
}

TEST(MovementSim, MovementCommandStopsOnCollisionPolicyStop) {
  MovementState previous;
  MovementCommand command;
  command.command_id = 46;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;
  command.collision_policy = MovementCommandCollisionPolicy::kStop;

  MovementConfig config;
  BlockingQuery query;
  auto result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 1000,
                                          config, query);

  EXPECT_FALSE(result.active);
  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.blocked);
  EXPECT_TRUE(result.collision_ended);
  EXPECT_EQ(result.command.elapsed_ms, 250u);
  EXPECT_NEAR(result.state.position.x, 2.5f, kEpsilon);
  EXPECT_NEAR(result.state.velocity.Length(), 0.0f, kEpsilon);

  command.collision_policy = MovementCommandCollisionPolicy::kEndSkill;
  result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 1000,
                                     config, query);

  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.collision_ended);
  EXPECT_NEAR(result.state.position.x, 2.5f, kEpsilon);
}

TEST(MovementSim, MovementCommandContinuePolicyIgnoresCollision) {
  MovementState previous;
  MovementCommand command;
  command.command_id = 47;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;
  command.collision_policy = MovementCommandCollisionPolicy::kContinue;

  MovementConfig config;
  BlockingQuery query;
  auto result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 500,
                                          config, query);

  EXPECT_TRUE(result.active);
  EXPECT_FALSE(result.blocked);
  EXPECT_FALSE(result.collision_ended);
  EXPECT_NEAR(result.state.position.x, 5.0f, kEpsilon);
}

TEST(MovementSim, MovementCommandUsesInjectedCollisionPolicy) {
  MovementState previous;
  MovementCommand command;
  command.command_id = 49;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;
  command.collision_policy = MovementCommandCollisionPolicy::kStop;

  MovementConfig config;
  BlockingQuery query;
  FixedCommandPolicy policy;
  auto result = ApplyMovementCommand(previous, command, LinearCurve(), 500, config,
                                     query, DefaultMovementCommandResolver(), policy);

  EXPECT_TRUE(result.active);
  EXPECT_FALSE(result.blocked);
  EXPECT_FALSE(result.collision_ended);
  EXPECT_NEAR(result.state.position.x, 5.0f, kEpsilon);
}

TEST(MovementSim, MovementCommandCurveCompletesAtTarget) {
  MovementState previous;
  previous.position = {7.5f, 0.0f, 0.0f};
  MovementCommand command;
  command.command_id = 45;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.elapsed_ms = 750;
  command.curve_id = 7;

  auto result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 300);

  EXPECT_FALSE(result.active);
  EXPECT_TRUE(result.completed);
  EXPECT_EQ(result.command.elapsed_ms, 1000u);
  EXPECT_NEAR(result.state.position.x, 10.0f, kEpsilon);
}

TEST(MovementSim, MovementCommandCurveRejectsInvalidCommand) {
  MovementState previous;
  previous.position = {3.0f, 0.0f, 0.0f};
  MovementCommand command;
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.curve_id = 7;

  auto result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 100);

  EXPECT_FALSE(IsMovementCommandValid(command, LinearCurve()));
  EXPECT_FALSE(result.active);
  EXPECT_TRUE(result.completed);
  EXPECT_NEAR(result.state.position.x, 3.0f, kEpsilon);
}

TEST(MovementSim, MovementCommandCurveRejectsZeroCommandId) {
  MovementState previous;
  previous.position = {3.0f, 0.0f, 0.0f};
  MovementCommand command;
  command.command_id = 0;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {10.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;

  auto result = ApplyMovementCommandCurve(previous, command, LinearCurve(), 100);

  EXPECT_FALSE(IsMovementCommandValid(command, LinearCurve()));
  EXPECT_FALSE(result.active);
  EXPECT_TRUE(result.completed);
  EXPECT_NEAR(result.state.position.x, 3.0f, kEpsilon);
}

TEST(MovementSim, MovementCurveStoreStoresValidCurve) {
  MovementCurveStore store;
  auto curve = LinearCurve(3);

  EXPECT_TRUE(store.Set(curve));

  const auto* stored = store.Find(3);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->id, 3u);
  EXPECT_EQ(stored->sample_count, 3u);
  EXPECT_FLOAT_EQ(SampleMovementCurve(*stored, 0.5f), 0.5f);
}

TEST(MovementSim, MovementCurveStoreRejectsInvalidCurve) {
  MovementCurveStore store;
  MovementCurve invalid;
  invalid.id = 5;

  EXPECT_FALSE(store.Set(invalid));
  EXPECT_EQ(store.Find(5), nullptr);
  EXPECT_EQ(store.Size(), 0u);
}

TEST(MovementSim, LinearMovementCurveFactoryCreatesIdentityCurve) {
  auto curve = MakeLinearMovementCurve(11);

  EXPECT_TRUE(IsMovementCurveValid(curve));
  EXPECT_EQ(curve.id, 11u);
  EXPECT_FLOAT_EQ(SampleMovementCurve(curve, 0.25f), 0.25f);
  EXPECT_FLOAT_EQ(SampleMovementCurve(curve, 0.75f), 0.75f);
}
