#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "movement_state_store.h"

using namespace atlas;

TEST(MovementStateStore, EnsureInitializesFromEntityPose) {
  MovementStateStore store;

  auto& state = store.Ensure(42, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 1.0f},
                             true);

  EXPECT_FLOAT_EQ(state.position.x, 1.0f);
  EXPECT_FLOAT_EQ(state.position.y, 2.0f);
  EXPECT_FLOAT_EQ(state.position.z, 3.0f);
  EXPECT_EQ(state.flags & movement::kMovementFlagGrounded,
            movement::kMovementFlagGrounded);
  EXPECT_EQ(store.Size(), 1u);
}

TEST(MovementStateStore, EnsureKeepsExistingState) {
  MovementStateStore store;
  auto& first = store.Ensure(42, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
                             true);
  first.position = {5.0f, 0.0f, 6.0f};
  first.last_processed_input_seq = 7;

  auto& second = store.Ensure(42, {99.0f, 0.0f, 99.0f}, {1.0f, 0.0f, 0.0f},
                              false);

  EXPECT_FLOAT_EQ(second.position.x, 5.0f);
  EXPECT_FLOAT_EQ(second.position.z, 6.0f);
  EXPECT_EQ(second.last_processed_input_seq, 7u);
}

TEST(MovementStateStore, AppendEntityIdsAndErase) {
  MovementStateStore store;
  (void)store.Ensure(2, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, true);
  (void)store.Ensure(5, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, true);

  std::vector<EntityID> ids;
  store.AppendEntityIds(ids);

  EXPECT_EQ(ids.size(), 2u);
  EXPECT_NE(std::find(ids.begin(), ids.end(), 2u), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), 5u), ids.end());

  store.Erase(2);
  EXPECT_EQ(store.Find(2), nullptr);
  ASSERT_NE(store.Find(5), nullptr);
}
