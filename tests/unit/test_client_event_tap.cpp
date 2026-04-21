#include <gtest/gtest.h>

#include "../../src/tools/world_stress/client_event_tap.h"

using namespace atlas::world_stress;

// ============================================================================
// Tracked-event cases — the exact log shapes produced by
// samples/client/StressAvatar.cs.
// ============================================================================

TEST(ClientEventTap, OnInit) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] OnInit", c));
  EXPECT_EQ(c.on_init, 1u);
  EXPECT_EQ(c.unparsed_lines, 0u);
}

TEST(ClientEventTap, OnEnterWorldWithPayload) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine(
      "[StressAvatar:42] OnEnterWorld pos=(1.00,2.00,3.00) hp=100", c));
  EXPECT_EQ(c.on_enter_world, 1u);
}

TEST(ClientEventTap, OnHpChanged) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] OnHpChanged old=100 new=99", c));
  EXPECT_EQ(c.on_hp_changed, 1u);
}

TEST(ClientEventTap, OnPositionUpdated) {
  ClientEventCounters c;
  EXPECT_TRUE(
      ParseAndCountClientEventLine("[StressAvatar:42] OnPositionUpdated pos=(1.50,2.00,3.00)", c));
  EXPECT_EQ(c.on_position_updated, 1u);
}

TEST(ClientEventTap, OnDestroy) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] OnDestroy", c));
  EXPECT_EQ(c.on_destroy, 1u);
}

// ============================================================================
// Unrelated / malformed inputs — harness must not crash or misattribute.
// ============================================================================

TEST(ClientEventTap, UnrelatedLogLine_BumpsUnparsed) {
  ClientEventCounters c;
  EXPECT_FALSE(ParseAndCountClientEventLine("Client: authenticated as entity=42 type=2", c));
  EXPECT_EQ(c.unparsed_lines, 1u);
  EXPECT_EQ(c.on_init, 0u);
}

TEST(ClientEventTap, EmptyLine_BumpsUnparsed) {
  ClientEventCounters c;
  EXPECT_FALSE(ParseAndCountClientEventLine("", c));
  EXPECT_EQ(c.unparsed_lines, 1u);
}

TEST(ClientEventTap, TokenPrefixDoesNotMatch) {
  // "OnInitExtra" must not match "OnInit" — token boundary required.
  ClientEventCounters c;
  EXPECT_FALSE(ParseAndCountClientEventLine("[StressAvatar:42] OnInitExtra foo", c));
  EXPECT_EQ(c.on_init, 0u);
  EXPECT_EQ(c.unparsed_lines, 1u);
}

TEST(ClientEventTap, NoBracket_BumpsUnparsed) {
  ClientEventCounters c;
  EXPECT_FALSE(ParseAndCountClientEventLine("OnInit", c));
  EXPECT_EQ(c.on_init, 0u);
  EXPECT_EQ(c.unparsed_lines, 1u);
}

TEST(ClientEventTap, MultipleLinesAccumulate) {
  ClientEventCounters c;
  (void)ParseAndCountClientEventLine("[Avatar:1] OnInit", c);
  (void)ParseAndCountClientEventLine("[Avatar:1] OnEnterWorld pos=(0,0,0) hp=100", c);
  (void)ParseAndCountClientEventLine("[Avatar:1] OnHpChanged old=100 new=99", c);
  (void)ParseAndCountClientEventLine("[Avatar:1] OnHpChanged old=99 new=98", c);
  (void)ParseAndCountClientEventLine("[Avatar:1] OnPositionUpdated pos=(1,0,0)", c);
  (void)ParseAndCountClientEventLine("random log line from engine", c);
  (void)ParseAndCountClientEventLine("[Avatar:1] OnDestroy", c);

  EXPECT_EQ(c.on_init, 1u);
  EXPECT_EQ(c.on_enter_world, 1u);
  EXPECT_EQ(c.on_hp_changed, 2u);
  EXPECT_EQ(c.on_position_updated, 1u);
  EXPECT_EQ(c.on_destroy, 1u);
  EXPECT_EQ(c.unparsed_lines, 1u);
}
