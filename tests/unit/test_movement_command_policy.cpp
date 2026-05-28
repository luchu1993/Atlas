#include <gtest/gtest.h>

#include "movement_sim/movement_sim.h"

using namespace atlas::movement;

namespace {

auto MakeCommand(MovementCommandInputPolicy input,
                 MovementCommandCollisionPolicy collision) -> MovementCommand {
  MovementCommand c;
  c.input_policy = input;
  c.collision_policy = collision;
  return c;
}

auto MakeStepResult(bool completed, bool collision_ended) -> MovementCommandStepResult {
  MovementCommandStepResult r;
  r.completed = completed;
  r.collision_ended = collision_ended;
  return r;
}

}  // namespace

TEST(DefaultMovementCommandPolicy, SuppressesInputOnlyForSuppressPolicy) {
  const auto& policy = DefaultMovementCommandPolicy();
  EXPECT_TRUE(policy.SuppressesInput(
      MakeCommand(MovementCommandInputPolicy::kSuppress,
                  MovementCommandCollisionPolicy::kStop)));
  EXPECT_FALSE(policy.SuppressesInput(
      MakeCommand(MovementCommandInputPolicy::kAllowTurn,
                  MovementCommandCollisionPolicy::kStop)));
  EXPECT_FALSE(policy.SuppressesInput(
      MakeCommand(MovementCommandInputPolicy::kAllowFull,
                  MovementCommandCollisionPolicy::kStop)));
}

TEST(DefaultMovementCommandPolicy, AllowsTurnInputOnlyForAllowTurnPolicy) {
  const auto& policy = DefaultMovementCommandPolicy();
  EXPECT_TRUE(policy.AllowsTurnInput(
      MakeCommand(MovementCommandInputPolicy::kAllowTurn,
                  MovementCommandCollisionPolicy::kStop)));
  EXPECT_FALSE(policy.AllowsTurnInput(
      MakeCommand(MovementCommandInputPolicy::kSuppress,
                  MovementCommandCollisionPolicy::kStop)));
  EXPECT_FALSE(policy.AllowsTurnInput(
      MakeCommand(MovementCommandInputPolicy::kAllowFull,
                  MovementCommandCollisionPolicy::kStop)));
}

TEST(DefaultMovementCommandPolicy, ChecksCollisionUnlessContinue) {
  const auto& policy = DefaultMovementCommandPolicy();
  EXPECT_TRUE(policy.ChecksCollision(
      MakeCommand(MovementCommandInputPolicy::kSuppress,
                  MovementCommandCollisionPolicy::kStop)));
  EXPECT_TRUE(policy.ChecksCollision(
      MakeCommand(MovementCommandInputPolicy::kSuppress,
                  MovementCommandCollisionPolicy::kEndSkill)));
  EXPECT_FALSE(policy.ChecksCollision(
      MakeCommand(MovementCommandInputPolicy::kSuppress,
                  MovementCommandCollisionPolicy::kContinue)));
}

TEST(DefaultMovementCommandPolicy, EndReasonForCollisionWins) {
  const auto& policy = DefaultMovementCommandPolicy();
  EXPECT_EQ(policy.EndReasonFor(MakeStepResult(true, true)),
            MovementCommandEndReason::kCollision);
}

TEST(DefaultMovementCommandPolicy, EndReasonForCompletedWithoutCollision) {
  const auto& policy = DefaultMovementCommandPolicy();
  EXPECT_EQ(policy.EndReasonFor(MakeStepResult(true, false)),
            MovementCommandEndReason::kCompleted);
}

TEST(DefaultMovementCommandPolicy, EndReasonForNeitherIsInvalid) {
  const auto& policy = DefaultMovementCommandPolicy();
  EXPECT_EQ(policy.EndReasonFor(MakeStepResult(false, false)),
            MovementCommandEndReason::kInvalid);
}

TEST(DefaultMovementCommandPolicy, PtrAndRefReferToSameInstance) {
  const auto& ref = DefaultMovementCommandPolicy();
  const auto ptr = DefaultMovementCommandPolicyPtr();
  EXPECT_EQ(&ref, ptr.get());
}
