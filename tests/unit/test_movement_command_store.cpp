#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "movement_command_store.h"

using namespace atlas;

namespace {

auto Command(uint32_t id = 1) -> movement::MovementCommand {
  movement::MovementCommand command;
  command.command_id = id;
  command.skill_id = 10;
  command.start_position = {0.0f, 0.0f, 0.0f};
  command.target_position = {5.0f, 0.0f, 0.0f};
  command.duration_ms = 1000;
  command.curve_id = 7;
  return command;
}

}  // namespace

TEST(MovementCommandStore, SetFindAndReplaceCommand) {
  MovementCommandStore store;

  EXPECT_TRUE(store.Set(42, Command(1)));
  auto replacement = Command(2);
  replacement.elapsed_ms = 250;
  EXPECT_TRUE(store.Set(42, replacement));

  const auto* found = store.Find(42);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->command_id, 2u);
  EXPECT_EQ(found->elapsed_ms, 250u);
  EXPECT_EQ(store.Size(), 1u);
}

TEST(MovementCommandStore, RejectsInvalidEntityAndCommand) {
  MovementCommandStore store;

  EXPECT_FALSE(store.Set(kInvalidEntityID, Command()));

  auto invalid = Command();
  invalid.command_id = 0;
  EXPECT_FALSE(store.Set(42, invalid));

  invalid = Command();
  invalid.duration_ms = 0;
  EXPECT_FALSE(store.Set(42, invalid));

  invalid = Command();
  invalid.elapsed_ms = 1001;
  EXPECT_FALSE(store.Set(42, invalid));

  invalid = Command();
  invalid.input_policy = movement::MovementCommandInputPolicy::kAllowFull;
  EXPECT_FALSE(store.Set(42, invalid));

  auto allow_turn = Command();
  allow_turn.input_policy = movement::MovementCommandInputPolicy::kAllowTurn;
  EXPECT_TRUE(store.Set(42, allow_turn));

  store.Erase(42);
  EXPECT_EQ(store.Size(), 0u);
}

TEST(MovementCommandStore, EraseDropsCommand) {
  MovementCommandStore store;
  ASSERT_TRUE(store.Set(42, Command()));

  store.Erase(42);

  EXPECT_EQ(store.Find(42), nullptr);
  EXPECT_EQ(store.Size(), 0u);
}

TEST(MovementCommandStore, AppendEntityIdsReturnsStoredCommands) {
  MovementCommandStore store;
  ASSERT_TRUE(store.Set(7, Command()));
  ASSERT_TRUE(store.Set(9, Command()));

  std::vector<EntityID> ids;
  ids.push_back(3);
  store.AppendEntityIds(ids);

  EXPECT_EQ(ids.size(), 3u);
  EXPECT_NE(std::find(ids.begin(), ids.end(), EntityID{7}), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), EntityID{9}), ids.end());
}
