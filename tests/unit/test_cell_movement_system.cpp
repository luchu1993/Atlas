#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "cell_movement_system.h"
#include "movement_sim/movement_codec.h"
#include "movement_sim/movement_curve_store.h"
#include "movement_sim/movement_sim.h"
#include "physics/physics_query.h"

using namespace atlas;
using namespace atlas::movement;

namespace {

class CapturingHost final : public CellMovementHost {
 public:
  explicit CapturingHost(bool actor_present = true) : actor_present_(actor_present) {}

  auto FindMovementActor(EntityID, MovementActorSnapshot& out) -> bool override {
    if (!actor_present_) return false;
    out.position = actor_position;
    out.direction = actor_direction;
    out.on_ground = actor_on_ground;
    out.physics_query = &query;
    return true;
  }
  auto MovementNow() -> TimePoint override { return now; }
  auto MovementServerTick() const -> uint32_t override { return server_tick; }
  void PublishMovementState(EntityID id, const MovementState& state) override {
    published_states.push_back({id, state});
  }
  void SendMovementStateAck(EntityID id, const MovementState& state, uint32_t tick) override {
    state_acks.push_back({id, tick, state});
  }
  void SendMovementCommandStart(EntityID id, const MovementCommand& command) override {
    command_starts.push_back({id, command});
  }
  void SendMovementCommandEnd(EntityID id, uint32_t command_id, const MovementState& state,
                              uint32_t tick, MovementCommandEndReason reason) override {
    command_ends.push_back({id, command_id, tick, reason, state});
  }

  bool actor_present_;
  math::Vector3 actor_position{0.0f, 0.0f, 0.0f};
  math::Vector3 actor_direction{0.0f, 0.0f, 1.0f};
  bool actor_on_ground{true};
  TimePoint now{};
  uint32_t server_tick{100};
  physics::NullPhysicsQuery query;

  struct StateUpdate {
    EntityID id;
    MovementState state;
  };
  struct AckCapture {
    EntityID id;
    uint32_t tick;
    MovementState state;
  };
  struct StartCapture {
    EntityID id;
    MovementCommand command;
  };
  struct EndCapture {
    EntityID id;
    uint32_t command_id;
    uint32_t tick;
    MovementCommandEndReason reason;
    MovementState state;
  };
  std::vector<StateUpdate> published_states;
  std::vector<AckCapture> state_acks;
  std::vector<StartCapture> command_starts;
  std::vector<EndCapture> command_ends;
};

auto MakeCommand(uint32_t id, uint16_t curve_id, uint8_t priority = 5,
                 MovementCommandInputPolicy input = MovementCommandInputPolicy::kAllowTurn)
    -> MovementCommand {
  MovementCommand c;
  c.command_id = id;
  c.skill_id = 1;
  c.type = MovementCommandType::kDash;
  c.start_position = {0.0f, 0.0f, 0.0f};
  c.target_position = {5.0f, 0.0f, 0.0f};
  c.duration_ms = 500;
  c.elapsed_ms = 0;
  c.curve_id = curve_id;
  c.input_policy = input;
  c.collision_policy = MovementCommandCollisionPolicy::kStop;
  c.priority = priority;
  c.server_tick = 0;
  return c;
}

}  // namespace

TEST(CellMovementSystem, SetCommandBroadcastsStartAndStampsServerTick) {
  CellMovementSystem system;
  CapturingHost host;
  host.server_tick = 4242;

  ASSERT_TRUE(system.SetCommand(host, 100, MakeCommand(7, 0)));

  ASSERT_EQ(host.command_starts.size(), 1u);
  EXPECT_EQ(host.command_starts[0].id, 100u);
  EXPECT_EQ(host.command_starts[0].command.command_id, 7u);
  EXPECT_EQ(host.command_starts[0].command.server_tick, 4242u);
  EXPECT_TRUE(host.command_ends.empty());
}

TEST(CellMovementSystem, SetCommandRejectsLowerPriorityWhenActive) {
  CellMovementSystem system;
  CapturingHost host;

  ASSERT_TRUE(system.SetCommand(host, 100, MakeCommand(7, 0, 10)));
  EXPECT_FALSE(system.SetCommand(host, 100, MakeCommand(8, 0, 5)));

  ASSERT_EQ(host.command_starts.size(), 1u);
  EXPECT_EQ(host.command_starts[0].command.command_id, 7u);
}

TEST(CellMovementSystem, SetCommandInterruptsLowerPriorityWithEndBroadcast) {
  CellMovementSystem system;
  CapturingHost host;

  ASSERT_TRUE(system.SetCommand(host, 100, MakeCommand(7, 0, 5)));
  ASSERT_TRUE(system.SetCommand(host, 100, MakeCommand(8, 0, 10)));

  ASSERT_EQ(host.command_starts.size(), 2u);
  EXPECT_EQ(host.command_starts[1].command.command_id, 8u);
  ASSERT_EQ(host.command_ends.size(), 1u);
  EXPECT_EQ(host.command_ends[0].command_id, 7u);
  EXPECT_EQ(host.command_ends[0].reason, MovementCommandEndReason::kCancelled);
}

TEST(CellMovementSystem, SetCommandRejectsUnknownCurveSilently) {
  CellMovementSystem system;
  CapturingHost host;

  EXPECT_FALSE(system.SetCommand(host, 100, MakeCommand(7, 999)));
  EXPECT_TRUE(host.command_starts.empty());
  EXPECT_TRUE(host.command_ends.empty());
}

TEST(CellMovementSystem, ClearCommandBroadcastsEndWithCurrentTick) {
  CellMovementSystem system;
  CapturingHost host;

  ASSERT_TRUE(system.SetCommand(host, 100, MakeCommand(7, 0)));
  host.server_tick = 9000;
  EXPECT_TRUE(system.ClearCommand(host, 100, 0));

  ASSERT_EQ(host.command_ends.size(), 1u);
  EXPECT_EQ(host.command_ends[0].command_id, 7u);
  EXPECT_EQ(host.command_ends[0].tick, 9000u);
  EXPECT_EQ(host.command_ends[0].reason, MovementCommandEndReason::kCancelled);
}

TEST(CellMovementSystem, ClearCommandRejectsMismatchedCommandId) {
  CellMovementSystem system;
  CapturingHost host;

  ASSERT_TRUE(system.SetCommand(host, 100, MakeCommand(7, 0)));
  EXPECT_FALSE(system.ClearCommand(host, 100, 999));
  EXPECT_TRUE(host.command_ends.empty());
}

TEST(CellMovementSystem, ClearCommandReturnsFalseWhenNoActorPresent) {
  CellMovementSystem system;
  CapturingHost host(false);

  EXPECT_FALSE(system.ClearCommand(host, 100, 0));
  EXPECT_TRUE(host.command_ends.empty());
}

TEST(CellMovementSystem, EnqueueClientInputDropsOversizedBatch) {
  CellMovementSystem system;
  CapturingHost host;

  std::vector<InputFrame> too_many(kMaxMovementInputFrames + 1);
  for (std::size_t i = 0; i < too_many.size(); ++i) {
    too_many[i].seq = static_cast<uint32_t>(i + 1);
    too_many[i].input_tick = static_cast<uint32_t>(i + 1);
    too_many[i].client_dt_ms = 33;
  }

  EXPECT_FALSE(system.EnqueueClientInput(host, 100, too_many));
}

TEST(CellMovementSystem, EnqueueClientInputDropsSuppressedCommand) {
  CellMovementSystem system;
  CapturingHost host;

  auto suppress = MakeCommand(7, 0, 5, MovementCommandInputPolicy::kSuppress);
  ASSERT_TRUE(system.SetCommand(host, 100, suppress));

  InputFrame frame{};
  frame.seq = 1;
  frame.input_tick = 1;
  frame.client_dt_ms = 33;
  std::vector<InputFrame> frames{frame};

  EXPECT_FALSE(system.EnqueueClientInput(host, 100, frames));
}

TEST(CellMovementSystem, TickPublishesStateAckOnInputDrain) {
  CellMovementSystem system;
  CapturingHost host;
  host.server_tick = 99;

  InputFrame frame{};
  frame.seq = 1;
  frame.input_tick = 1;
  frame.client_dt_ms = 33;
  std::vector<InputFrame> frames{frame};
  ASSERT_TRUE(system.EnqueueClientInput(host, 100, frames));

  system.Tick(host, 0.033f);

  ASSERT_EQ(host.state_acks.size(), 1u);
  EXPECT_EQ(host.state_acks[0].id, 100u);
  EXPECT_EQ(host.state_acks[0].state.last_processed_input_seq, 1u);
}

TEST(CellMovementSystem, PositionHistoryRecordAcceptsTickAcrossU32Wrap) {
  CellMovementSystem system;
  CapturingHost host;
  MovementState state{};
  state.position = {0.0f, 0.0f, 1.0f};
  // Seed history just below the wrap boundary; then a sample just past
  // the wrap should still be accepted as "newer" via signed-delta logic.
  const uint32_t near_max = std::numeric_limits<uint32_t>::max() - 1u;
  system.position_history().Record(99u, near_max, state);
  system.position_history().Record(99u, near_max + 1u, state);  // == max
  system.position_history().Record(99u, near_max + 2u, state);  // wrapped to 0
  system.position_history().Record(99u, 1u, state);
  const auto* history = system.position_history().Find(99u);
  ASSERT_NE(history, nullptr);
  // 4 distinct samples logged across the wrap; the last one's raw tick
  // value is 1 even though it's the newest in time-order.
  EXPECT_EQ(history->size(), 4u);
  EXPECT_EQ(history->back().server_tick, 1u);
}

TEST(CellMovementSystem, SetCommandSameIdReturnsTrueWithoutResettingElapsedMs) {
  CellMovementSystem system;
  MovementCurve linear{};
  linear.id = 1;
  linear.sample_count = 2;
  linear.samples[0] = 0.0f;
  linear.samples[1] = 1.0f;
  ASSERT_TRUE(system.SetCurve(linear));
  CapturingHost host;
  auto cmd = MakeCommand(/*id=*/77u, /*curve_id=*/1u);
  cmd.target_position = {0.05f, 0.0f, 0.0f};
  cmd.duration_ms = 500u;
  ASSERT_TRUE(system.SetCommand(host, 300u, cmd));
  const auto* active = system.command_store().Find(300u);
  ASSERT_NE(active, nullptr);
  // Simulate the active command having accumulated some progress.
  auto progressed = *active;
  progressed.elapsed_ms = 200u;
  ASSERT_TRUE(system.command_store().Set(300u, progressed));
  host.command_starts.clear();

  // A resend of the same command_id (with elapsed_ms = 0 as the caller would
  // pass) must NOT reset progress and must NOT emit another Start broadcast.
  auto resend = cmd;
  resend.start_position = {3.0f, 0.0f, 4.0f};  // hostile overwrite attempt
  resend.target_position = {99.0f, 0.0f, 99.0f};
  resend.elapsed_ms = 0u;
  EXPECT_TRUE(system.SetCommand(host, 300u, resend));
  const auto* after = system.command_store().Find(300u);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->command_id, 77u);
  EXPECT_EQ(after->elapsed_ms, 200u);
  EXPECT_FLOAT_EQ(after->target_position.x, 0.05f);
  EXPECT_TRUE(host.command_starts.empty());
}

TEST(CellMovementSystem, SubMillisecondTickAccumulatesResidueWithoutAdvancingCommand) {
  CellMovementSystem system;
  MovementCurve linear{};
  linear.id = 1;
  linear.sample_count = 2;
  linear.samples[0] = 0.0f;
  linear.samples[1] = 1.0f;
  ASSERT_TRUE(system.SetCurve(linear));
  CapturingHost host;

  // Short target distance keeps the implied velocity well under any speed
  // limit so the command isn't erased mid-test for an unrelated reason.
  auto cmd = MakeCommand(/*id=*/1u, /*curve_id=*/1u);
  cmd.target_position = {0.05f, 0.0f, 0.0f};
  cmd.duration_ms = 500u;
  ASSERT_TRUE(system.SetCommand(host, 200u, cmd));
  ASSERT_NE(system.command_store().Find(200u), nullptr);
  const uint16_t initial_elapsed = system.command_store().Find(200u)->elapsed_ms;

  // 4 ticks of 0.3ms each = 1.2ms accumulated. The first 3 should be
  // residue-only (no command advance, elapsed unchanged); the 4th will see
  // 1.2ms total and advance by 1ms, leaving 0.2ms in residue.
  for (int i = 0; i < 3; ++i) system.Tick(host, 0.0003f);
  EXPECT_EQ(system.command_store().Find(200u)->elapsed_ms, initial_elapsed)
      << "command should not advance on sub-ms ticks";

  system.Tick(host, 0.0003f);
  const auto* after = system.command_store().Find(200u);
  ASSERT_NE(after, nullptr);
  EXPECT_GE(after->elapsed_ms, initial_elapsed + 1u);
}

TEST(CellMovementSystem, RestorePositionHistoryRebasesToDestTick) {
  CellMovementSystem system;
  CapturingHost host;
  // Source samples carry source-cell ticks; rebase puts the latest at
  // dest_tick and shifts the rest by the same offset.
  MovementState s1{};
  s1.position = {0.0f, 0.0f, 1.0f};
  MovementState s2{};
  s2.position = {0.0f, 0.0f, 2.0f};
  MovementState s3{};
  s3.position = {0.0f, 0.0f, 3.0f};
  std::vector<MovementPositionSample> samples{
      {500u, s1}, {501u, s2}, {502u, s3}};

  const uint32_t dest_tick = 1000u;
  system.RestorePositionHistoryFromOffload(99u, dest_tick, samples);
  const auto* history = system.position_history().Find(99u);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), 3u);
  EXPECT_EQ(history->back().server_tick, dest_tick);
  EXPECT_FLOAT_EQ(history->back().state.position.z, 3.0f);
  EXPECT_EQ((*history)[1].server_tick, dest_tick - 1u);
  EXPECT_EQ((*history)[0].server_tick, dest_tick - 2u);
}

TEST(CellMovementSystem, RestorePositionHistoryDropsSamplesThatWouldGoNegative) {
  CellMovementSystem system;
  MovementState s{};
  s.position = {0.0f, 0.0f, 1.0f};
  // Source max = 100, dest = 2 → offset = -98; samples 0..97 underflow,
  // 98..100 land at dest ticks 0..2.
  std::vector<MovementPositionSample> samples{
      {0u, s}, {50u, s}, {98u, s}, {99u, s}, {100u, s}};

  system.RestorePositionHistoryFromOffload(99u, /*current_server_tick=*/2u, samples);
  const auto* history = system.position_history().Find(99u);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), 3u);
  EXPECT_EQ((*history)[0].server_tick, 0u);
  EXPECT_EQ((*history)[1].server_tick, 1u);
  EXPECT_EQ((*history)[2].server_tick, 2u);
}

TEST(CellMovementSystem, RestorePositionHistoryAdmitsSubsequentRecordAtDestTick) {
  CellMovementSystem system;
  CapturingHost host;
  host.server_tick = 50u;  // dest is behind the source samples
  MovementState s{};
  s.position = {0.0f, 0.0f, 1.0f};
  std::vector<MovementPositionSample> samples{{200u, s}, {201u, s}};
  system.RestorePositionHistoryFromOffload(99u, host.server_tick, samples);

  // Drive a Tick → host.PublishMovementState → position_history_.Record at
  // dest tick 50. Without rebase, the new sample (server_tick=50) would be
  // dropped because the restored back() would carry source's 201.
  MovementState state{};
  state.position = {0.0f, 0.0f, 5.0f};
  EXPECT_TRUE(system.RestoreState(99u, state));
  system.Tick(host, 0.033f);
  const auto* history = system.position_history().Find(99u);
  ASSERT_NE(history, nullptr);
  EXPECT_GE(history->size(), 2u);
  EXPECT_EQ(history->back().server_tick, host.server_tick);
}
