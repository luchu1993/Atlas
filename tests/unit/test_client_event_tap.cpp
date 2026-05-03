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
// Component-coverage events (struct props, struct-arg RPC, list-arg RPC,
// component-level RPC).
// ============================================================================

TEST(ClientEventTap, OnMainWeaponChanged) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine(
      "[StressAvatar:42] OnMainWeaponChanged old.id=999 new.id=1000", c));
  EXPECT_EQ(c.on_main_weapon_changed, 1u);
}

TEST(ClientEventTap, OnWeaponBroken) {
  ClientEventCounters c;
  EXPECT_TRUE(
      ParseAndCountClientEventLine("[StressAvatar:42] OnWeaponBroken id=42 sharpness=10", c));
  EXPECT_EQ(c.on_weapon_broken, 1u);
}

TEST(ClientEventTap, OnScoresSnapshot) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] OnScoresSnapshot count=5", c));
  EXPECT_EQ(c.on_scores_snapshot, 1u);
}

TEST(ClientEventTap, OnAffixesUpdated) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressLoadComponent:42] OnAffixesUpdated count=3", c));
  EXPECT_EQ(c.on_affixes_updated, 1u);
}

TEST(ClientEventTap, OnAreaBroadcast) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] OnAreaBroadcast seq=1 payload=3", c));
  EXPECT_EQ(c.on_area_broadcast, 1u);
  EXPECT_EQ(c.unparsed_lines, 0u);
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

// ============================================================================
// event_seq gap warnings
// ============================================================================

TEST(ClientEventTap, EventSeqGap_AddsMissedCount) {
  ClientEventCounters c;
  EXPECT_TRUE(
      ParseAndCountClientEventLine("[StressAvatar:42] event_seq gap: last=10 got=15 missed=4", c));
  EXPECT_EQ(c.event_seq_gaps, 4u);
  EXPECT_EQ(c.unparsed_lines, 0u);
}

TEST(ClientEventTap, EventSeqGap_MultipleLinesAccumulate) {
  ClientEventCounters c;
  (void)ParseAndCountClientEventLine("[StressAvatar:42] event_seq gap: last=10 got=15 missed=4", c);
  (void)ParseAndCountClientEventLine("[StressAvatar:42] event_seq gap: last=15 got=20 missed=4", c);
  (void)ParseAndCountClientEventLine("[StressAvatar:99] event_seq gap: last=3 got=7 missed=3", c);
  EXPECT_EQ(c.event_seq_gaps, 11u);
}

TEST(ClientEventTap, EventSeqGap_MissingMissedField_CountsLineButNoMiss) {
  // Defensive — a future refactor removing the `missed=` suffix should
  // surface as a parser warning, not as silently mis-counted deltas.
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] event_seq gap: last=10 got=15", c));
  EXPECT_EQ(c.event_seq_gaps, 0u);
  EXPECT_EQ(c.unparsed_lines, 0u);  // well-formed prefix → recognized
}

// ============================================================================
// L6 — optional [t=...] timestamp prefix (ClientLog in Atlas.Client)
// ============================================================================

TEST(ClientEventTap, TimestampPrefix_Stripped_OnInit) {
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[t=1.234] [StressAvatar:42] OnInit", c));
  EXPECT_EQ(c.on_init, 1u);
  EXPECT_EQ(c.unparsed_lines, 0u);
}

TEST(ClientEventTap, TimestampPrefix_Stripped_HpChanged) {
  ClientEventCounters c;
  EXPECT_TRUE(
      ParseAndCountClientEventLine("[t=12.345] [StressAvatar:42] OnHpChanged old=100 new=99", c));
  EXPECT_EQ(c.on_hp_changed, 1u);
}

TEST(ClientEventTap, TimestampPrefix_Stripped_EventSeqGap) {
  // Both the event_seq gap warning (stderr) and the regular events
  // (stdout) carry the prefix because ClientLog.Warn/Info share the
  // same Timestamp() helper. Missing any one path would desync the
  // harness counter from the on-wire state.
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine(
      "[t=5.000] [StressAvatar:42] event_seq gap: last=10 got=15 missed=4", c));
  EXPECT_EQ(c.event_seq_gaps, 4u);
}

TEST(ClientEventTap, TimestampPrefix_MissingClose_BumpsUnparsed) {
  // Malformed: opens [t= but never closes. Defensively treated as
  // unparsed — we don't want a partial parse to shadow a later
  // identifiable event on the same line.
  ClientEventCounters c;
  EXPECT_FALSE(ParseAndCountClientEventLine("[t=1.234 [StressAvatar:42] OnInit", c));
  EXPECT_EQ(c.on_init, 0u);
  EXPECT_EQ(c.unparsed_lines, 1u);
}

TEST(ClientEventTap, PreL6Format_StillParses) {
  // Backward compat: a log line without the timestamp bracket must
  // continue to match exactly as it did before L6 landed. Pre-L6
  // builds shipped without the prefix and runs that mix old client
  // binaries with a new tap must not regress.
  ClientEventCounters c;
  EXPECT_TRUE(ParseAndCountClientEventLine("[StressAvatar:42] OnHpChanged old=100 new=99", c));
  EXPECT_EQ(c.on_hp_changed, 1u);
}
