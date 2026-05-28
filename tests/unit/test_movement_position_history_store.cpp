#include <gtest/gtest.h>

#include "movement_position_history_store.h"

using namespace atlas;

namespace {

auto State(float x, uint32_t flags = 0, uint32_t seq = 0) -> movement::MovementState {
  movement::MovementState state;
  state.position = {x, 0.0f, 0.0f};
  state.velocity = {0.0f, 0.0f, x};
  state.direction = {0.0f, 0.0f, 1.0f};
  state.flags = flags;
  state.last_processed_input_seq = seq;
  return state;
}

}  // namespace

TEST(MovementPositionHistoryStore, RecordsAndCapsSamplesPerEntity) {
  MovementPositionHistoryStore store(3);

  store.Record(42, 1, State(1.0f));
  store.Record(42, 2, State(2.0f));
  store.Record(42, 3, State(3.0f));
  store.Record(42, 4, State(4.0f));

  const auto* history = store.Find(42);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), 3u);
  EXPECT_EQ(history->front().server_tick, 2u);
  EXPECT_EQ(history->back().server_tick, 4u);
  EXPECT_EQ(store.EntityCount(), 1u);
  EXPECT_EQ(store.TotalSampleCount(), 3u);
}

TEST(MovementPositionHistoryStore, ReplacesSameTickAndIgnoresOlderTick) {
  MovementPositionHistoryStore store;

  store.Record(42, 10, State(1.0f));
  store.Record(42, 10, State(2.0f));
  store.Record(42, 9, State(9.0f));

  const auto* history = store.Find(42);
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_FLOAT_EQ(history->back().state.position.x, 2.0f);
}

TEST(MovementPositionHistoryStore, SampleAtInterpolatesInsideHistoryWindow) {
  MovementPositionHistoryStore store;
  auto before = State(0.0f, movement::kMovementFlagGrounded, 7);
  before.direction = {0.0f, 0.0f, 1.0f};
  auto after = State(10.0f, 0, 8);
  after.velocity = {0.0f, 0.0f, 20.0f};
  after.direction = {1.0f, 0.0f, 0.0f};

  store.Record(42, 10, before);
  store.Record(42, 20, after);

  auto sample = store.SampleAt(42, 15);
  ASSERT_TRUE(sample.has_value());
  EXPECT_EQ(sample->server_tick, 15u);
  EXPECT_FLOAT_EQ(sample->state.position.x, 5.0f);
  EXPECT_FLOAT_EQ(sample->state.velocity.z, 10.0f);
  EXPECT_NEAR(sample->state.direction.x, 0.70710677f, 0.0001f);
  EXPECT_NEAR(sample->state.direction.z, 0.70710677f, 0.0001f);
  EXPECT_EQ(sample->state.flags, 0u);
  EXPECT_EQ(sample->state.last_processed_input_seq, 8u);

  EXPECT_FALSE(store.SampleAt(42, 9).has_value());
  EXPECT_FALSE(store.SampleAt(42, 21).has_value());
}

TEST(MovementPositionHistoryStore, EraseDropsEntityHistory) {
  MovementPositionHistoryStore store;
  store.Record(42, 1, State(1.0f));

  store.Erase(42);

  EXPECT_EQ(store.Find(42), nullptr);
  EXPECT_EQ(store.EntityCount(), 0u);
  EXPECT_EQ(store.TotalSampleCount(), 0u);
}
