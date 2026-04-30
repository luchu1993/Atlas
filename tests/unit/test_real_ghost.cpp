// CellEntity Real/Ghost + RealEntityData.
//
// Exercises the dual-mode CellEntity surface without spinning up CellApp,
// BaseApp, or a real network. Fake Channel* values are only ever compared
// for identity, never dereferenced — RealEntityData treats Channel as an
// opaque handle in its data path (actual SendMessage is CellApp's job).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "real_entity_data.h"
#include "space.h"

namespace atlas {
namespace {

// Fake Channel* used only for identity comparisons. Never dereferenced.
auto FakeChannel(uintptr_t tag) -> Channel* {
  return reinterpret_cast<Channel*>(tag);
}

auto MakeBlob(std::initializer_list<uint8_t> bytes) -> std::vector<std::byte> {
  std::vector<std::byte> v;
  v.reserve(bytes.size());
  for (auto b : bytes) v.push_back(static_cast<std::byte>(b));
  return v;
}

// ============================================================================
// Constructor default (implicit Real)
// ============================================================================

TEST(RealGhost, DefaultCtorStartsAsReal) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  EXPECT_TRUE(e->IsReal());
  EXPECT_FALSE(e->IsGhost());
  EXPECT_NE(e->GetRealData(), nullptr);
  EXPECT_EQ(e->GetRealChannel(), nullptr);
}

TEST(RealGhost, GhostTagCtorStartsAsGhost) {
  Space space(1);
  auto* c = FakeChannel(0xBEEF);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(CellEntity::GhostTag{}, 2, uint16_t{1},
                                                         space, math::Vector3{10, 0, 10},
                                                         math::Vector3{1, 0, 0}, c));
  EXPECT_FALSE(e->IsReal());
  EXPECT_TRUE(e->IsGhost());
  EXPECT_EQ(e->GetRealData(), nullptr);
  EXPECT_EQ(e->GetRealChannel(), c);
}

// ============================================================================
// Real → Ghost conversion
// ============================================================================

TEST(RealGhost, ConvertRealToGhost_DropsRealDataAndInstallsChannel) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      3, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  ASSERT_TRUE(e->IsReal());
  auto* new_channel = FakeChannel(0xCAFE);
  e->ConvertRealToGhost(new_channel);
  EXPECT_FALSE(e->IsReal());
  EXPECT_TRUE(e->IsGhost());
  EXPECT_EQ(e->GetRealData(), nullptr);
  EXPECT_EQ(e->GetRealChannel(), new_channel);
}

TEST(RealGhost, ConvertRealToGhost_IgnoredOnGhost) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 4, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0},
      FakeChannel(0xAA)));
  e->ConvertRealToGhost(FakeChannel(0xBB));
  // Original Ghost channel untouched.
  EXPECT_EQ(e->GetRealChannel(), FakeChannel(0xAA));
}

// ============================================================================
// RebindRealChannel
// ============================================================================

TEST(RealGhost, RebindRealChannel_UpdatesBackChannelAndClearsNextRealAddr) {
  Space space(1);
  auto* initial_ch = FakeChannel(0xAA);
  auto* new_ch = FakeChannel(0xBB);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(CellEntity::GhostTag{}, 42, uint16_t{1},
                                                         space, math::Vector3{0, 0, 0},
                                                         math::Vector3{1, 0, 0}, initial_ch));
  // Simulate mid-Offload state: pre-reg next_real_addr via GhostSetNextReal.
  e->SetNextRealAddr(Address(0x7F000001u, 30002));
  ASSERT_EQ(e->GetRealChannel(), initial_ch);

  e->RebindRealChannel(new_ch);
  EXPECT_EQ(e->GetRealChannel(), new_ch);
  // Offload handoff complete → next_real_addr cleared.
  EXPECT_EQ(e->NextRealAddr().Ip(), 0u);
  EXPECT_EQ(e->NextRealAddr().Port(), 0u);
}

TEST(RealGhost, RebindRealChannel_IgnoredOnReal) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      99, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  ASSERT_TRUE(e->IsReal());
  auto* new_ch = FakeChannel(0xBB);
  e->RebindRealChannel(new_ch);
  // Real has no back-channel, and RebindRealChannel must NOT install one.
  EXPECT_EQ(e->GetRealChannel(), nullptr);
  EXPECT_TRUE(e->IsReal());
}

// ============================================================================
// Ghost → Real conversion
// ============================================================================

TEST(RealGhost, ConvertGhostToReal_ClearsChannelAndReattachesSidecar) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 5, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0},
      FakeChannel(0xAA)));
  ASSERT_TRUE(e->IsGhost());
  e->ConvertGhostToReal();
  EXPECT_TRUE(e->IsReal());
  EXPECT_FALSE(e->IsGhost());
  EXPECT_EQ(e->GetRealChannel(), nullptr);
  EXPECT_NE(e->GetRealData(), nullptr);
}

TEST(RealGhost, ConvertGhostToReal_PreservesReplicationBaseline) {
  // Round-trip a baseline through a GhostApplySnapshot, then promote.
  // The new Real must inherit the snapshot so it can keep serving peers
  // until the C# layer publishes the first post-offload frame.
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 6, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0},
      FakeChannel(0xAA)));
  auto snapshot = MakeBlob({0xDE, 0xAD, 0xBE, 0xEF});
  e->GhostApplySnapshot(100, std::span<const std::byte>(snapshot));
  ASSERT_NE(e->GetReplicationState(), nullptr);
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 100u);
  EXPECT_EQ(e->GetReplicationState()->other_snapshot.size(), 4u);

  e->ConvertGhostToReal();
  ASSERT_NE(e->GetReplicationState(), nullptr);
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 100u);
  EXPECT_EQ(e->GetReplicationState()->other_snapshot.size(), 4u);
}

// ============================================================================
// GhostUpdatePosition — latest-wins volatile stream
// ============================================================================

TEST(RealGhost, GhostUpdatePosition_AppliesWhenSeqAdvances) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 7, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0},
      FakeChannel(0xAA)));
  e->GhostUpdatePosition(math::Vector3{5, 0, 7}, math::Vector3{0, 0, 1}, /*on_ground=*/true, 1);
  EXPECT_FLOAT_EQ(e->Position().x, 5.f);
  EXPECT_FLOAT_EQ(e->Position().z, 7.f);
  EXPECT_FLOAT_EQ(e->Direction().z, 1.f);
  EXPECT_TRUE(e->OnGround());
  ASSERT_NE(e->GetReplicationState(), nullptr);
  EXPECT_EQ(e->GetReplicationState()->latest_volatile_seq, 1u);
  // RangeList kept in sync for peer AoI lookups.
  EXPECT_FLOAT_EQ(e->RangeNode().X(), 5.f);
  EXPECT_FLOAT_EQ(e->RangeNode().Z(), 7.f);
}

TEST(RealGhost, GhostUpdatePosition_IgnoresStaleSeq) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 8, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0},
      FakeChannel(0xAA)));
  e->GhostUpdatePosition({5, 0, 7}, {0, 0, 1}, true, 10);
  e->GhostUpdatePosition({99, 0, 99}, {1, 0, 0}, false, 5);  // stale — drop
  EXPECT_FLOAT_EQ(e->Position().x, 5.f);
  EXPECT_EQ(e->GetReplicationState()->latest_volatile_seq, 10u);
}

TEST(RealGhost, GhostUpdatePosition_RejectsOnReal) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      9, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  e->GhostUpdatePosition({99, 0, 99}, {0, 0, 1}, true, 1);
  EXPECT_FLOAT_EQ(e->Position().x, 0.f);
  EXPECT_EQ(e->GetReplicationState(), nullptr);
}

// ============================================================================
// GhostApplyDelta — cumulative event_seq with history window
// ============================================================================

TEST(RealGhost, GhostApplyDelta_AppendsToHistoryAndAdvancesSeq) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 10, uint16_t{1}, space, math::Vector3{0, 0, 0},
      math::Vector3{1, 0, 0}, FakeChannel(0xAA)));
  auto d1 = MakeBlob({0x01});
  auto d2 = MakeBlob({0x02, 0x03});
  e->GhostApplyDelta(1, std::span<const std::byte>(d1));
  e->GhostApplyDelta(2, std::span<const std::byte>(d2));
  ASSERT_NE(e->GetReplicationState(), nullptr);
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 2u);
  ASSERT_EQ(e->GetReplicationState()->history.size(), 2u);
  EXPECT_EQ(e->GetReplicationState()->history[0].other_delta.size(), 1u);
  EXPECT_EQ(e->GetReplicationState()->history[1].other_delta.size(), 2u);
}

TEST(RealGhost, GhostApplyDelta_DropsStaleOrDuplicate) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 11, uint16_t{1}, space, math::Vector3{0, 0, 0},
      math::Vector3{1, 0, 0}, FakeChannel(0xAA)));
  auto d = MakeBlob({0xFF});
  e->GhostApplyDelta(5, std::span<const std::byte>(d));
  e->GhostApplyDelta(5, std::span<const std::byte>(d));  // duplicate — ignored
  e->GhostApplyDelta(3, std::span<const std::byte>(d));  // stale — ignored
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 5u);
  EXPECT_EQ(e->GetReplicationState()->history.size(), 1u);
}

TEST(RealGhost, GhostApplyDelta_HistoryBoundedByReplicationWindow) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 12, uint16_t{1}, space, math::Vector3{0, 0, 0},
      math::Vector3{1, 0, 0}, FakeChannel(0xAA)));
  auto d = MakeBlob({0xAB});
  // Push kReplicationHistoryWindow + 3 deltas; expect only the last WINDOW
  // to remain.
  const size_t window = CellEntity::kReplicationHistoryWindow;
  for (uint64_t seq = 1; seq <= window + 3; ++seq) {
    e->GhostApplyDelta(seq, std::span<const std::byte>(d));
  }
  EXPECT_EQ(e->GetReplicationState()->history.size(), window);
  EXPECT_EQ(e->GetReplicationState()->history.front().event_seq, 4u);  // oldest kept
  EXPECT_EQ(e->GetReplicationState()->history.back().event_seq, window + 3);
}

// ============================================================================
// GhostApplySnapshot — rebases baseline and clears history
// ============================================================================

TEST(RealGhost, GhostApplySnapshot_ReplacesBaselineAndClearsHistory) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 13, uint16_t{1}, space, math::Vector3{0, 0, 0},
      math::Vector3{1, 0, 0}, FakeChannel(0xAA)));
  auto d = MakeBlob({0x00});
  e->GhostApplyDelta(1, std::span<const std::byte>(d));
  e->GhostApplyDelta(2, std::span<const std::byte>(d));
  ASSERT_FALSE(e->GetReplicationState()->history.empty());

  auto snap = MakeBlob({0xAA, 0xBB});
  e->GhostApplySnapshot(100, std::span<const std::byte>(snap));
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 100u);
  EXPECT_EQ(e->GetReplicationState()->other_snapshot.size(), 2u);
  EXPECT_TRUE(e->GetReplicationState()->history.empty());
}

// ============================================================================
// RealEntityData — Haunt list
// ============================================================================

TEST(RealEntityData, AddHauntIsIdempotent) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      20, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto* rd = e->GetRealData();
  ASSERT_NE(rd, nullptr);
  auto* c1 = FakeChannel(0x11);
  auto* c2 = FakeChannel(0x22);
  const Address a1(0x7F000001u, 1);
  const Address a2(0x7F000001u, 2);
  EXPECT_TRUE(rd->AddHaunt(c1, a1));
  EXPECT_FALSE(rd->AddHaunt(c1, a1));  // duplicate
  EXPECT_TRUE(rd->AddHaunt(c2, a2));
  EXPECT_EQ(rd->HauntCount(), 2u);
  EXPECT_TRUE(rd->HasHaunt(c1));
  EXPECT_TRUE(rd->HasHaunt(c2));
}

TEST(RealEntityData, AddHauntRejectsNullptr) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      21, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  EXPECT_FALSE(e->GetRealData()->AddHaunt(nullptr, Address{}));
  EXPECT_EQ(e->GetRealData()->HauntCount(), 0u);
}

TEST(RealEntityData, RemoveHauntSwapBack) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      22, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto* rd = e->GetRealData();
  auto* c1 = FakeChannel(0x11);
  auto* c2 = FakeChannel(0x22);
  auto* c3 = FakeChannel(0x33);
  const Address a1(0x7F000001u, 1);
  const Address a2(0x7F000001u, 2);
  const Address a3(0x7F000001u, 3);
  rd->AddHaunt(c1, a1);
  rd->AddHaunt(c2, a2);
  rd->AddHaunt(c3, a3);
  EXPECT_TRUE(rd->RemoveHaunt(c2));
  EXPECT_EQ(rd->HauntCount(), 2u);
  EXPECT_FALSE(rd->HasHaunt(c2));
  EXPECT_TRUE(rd->HasHaunt(c1));
  EXPECT_TRUE(rd->HasHaunt(c3));
  EXPECT_FALSE(rd->RemoveHaunt(c2));  // already gone
}

// ============================================================================
// RealEntityData — message builders
// ============================================================================

TEST(RealEntityData, BuildPositionUpdate_ReflectsOwnerState) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      30, uint16_t{1}, space, math::Vector3{3, 4, 5}, math::Vector3{0, 0, 1}));
  e->SetOnGround(true);
  // Seed a volatile_seq via PublishReplicationFrame so BuildPositionUpdate
  // has a non-zero seq to copy.
  CellEntity::ReplicationFrame frame;
  frame.volatile_seq = 42;
  frame.position = e->Position();
  frame.direction = e->Direction();
  frame.on_ground = e->OnGround();
  e->PublishReplicationFrame(std::move(frame), {}, {});

  auto msg = e->GetRealData()->BuildPositionUpdate();
  EXPECT_EQ(msg.entity_id, 30u);
  EXPECT_FLOAT_EQ(msg.position.x, 3.f);
  EXPECT_FLOAT_EQ(msg.position.y, 4.f);
  EXPECT_FLOAT_EQ(msg.position.z, 5.f);
  EXPECT_FLOAT_EQ(msg.direction.z, 1.f);
  EXPECT_TRUE(msg.on_ground);
  EXPECT_EQ(msg.volatile_seq, 42u);
}

TEST(RealEntityData, BuildDelta_ForwardsLatestHistoryFrame) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      31, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto d1 = MakeBlob({0x01});
  auto d2 = MakeBlob({0x02, 0x02});
  CellEntity::ReplicationFrame f1;
  f1.event_seq = 1;
  f1.other_delta = d1;
  e->PublishReplicationFrame(std::move(f1), {}, {});
  CellEntity::ReplicationFrame f2;
  f2.event_seq = 2;
  f2.other_delta = d2;
  e->PublishReplicationFrame(std::move(f2), {}, {});

  auto msg = e->GetRealData()->BuildDelta();
  EXPECT_EQ(msg.entity_id, 31u);
  EXPECT_EQ(msg.event_seq, 2u);
  ASSERT_EQ(msg.other_delta.size(), 2u);
  EXPECT_EQ(msg.other_delta[0], std::byte{0x02});
}

TEST(RealEntityData, BuildDelta_EmptyWhenNoHistory) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      32, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto msg = e->GetRealData()->BuildDelta();
  EXPECT_EQ(msg.event_seq, 0u);
  EXPECT_TRUE(msg.other_delta.empty());
}

// Defend against a divergence between history.back().event_seq and
// state->latest_event_seq. The invariant holds under normal publish flow,
// but we'd rather produce an empty delta (letting the next pump upgrade
// to snapshot-refresh via gap > 1) than forward a corrupted pair of
// (wire seq, other_delta bytes) that don't describe the same frame.
TEST(RealEntityData, BuildDelta_EmptyWhenHistoryBackSeqMismatchesLatest) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      200, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  // Publish a real frame so history has content + state->latest_event_seq == 5.
  auto real_delta = MakeBlob({0x01, 0xAB, 0xCD});
  CellEntity::ReplicationFrame f;
  f.event_seq = 5;
  f.other_delta = real_delta;
  e->PublishReplicationFrame(std::move(f), {}, {});

  // Manually desynchronise — simulate a pathological state where the
  // back frame's seq no longer matches latest. The Ghost pump must
  // refuse to forward the stale delta under that seq.
  auto& mutable_state = e->GetReplicationStateMutableForTest();
  mutable_state.history.back().event_seq = 3;  // pretend back is older

  auto msg = e->GetRealData()->BuildDelta();
  EXPECT_EQ(msg.event_seq, 5u);
  EXPECT_TRUE(msg.other_delta.empty())
      << "mismatched seq must produce an empty delta so the pump escalates "
         "to GhostSnapshotRefresh rather than lying about the frame";
}

TEST(RealEntityData, BuildSnapshotRefresh_ReflectsOwnerSnapshot) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      33, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto snap = MakeBlob({0xCA, 0xFE});
  CellEntity::ReplicationFrame f;
  f.event_seq = 7;
  e->PublishReplicationFrame(std::move(f), {}, std::span<const std::byte>(snap));

  auto msg = e->GetRealData()->BuildSnapshotRefresh();
  EXPECT_EQ(msg.entity_id, 33u);
  EXPECT_EQ(msg.event_seq, 7u);
  ASSERT_EQ(msg.other_snapshot.size(), 2u);
  EXPECT_EQ(msg.other_snapshot[0], std::byte{0xCA});
}

// Empty-delta skip helper. All-zero or truly-empty payloads carry no
// audience-visible content and must not incur per-haunt wire traffic
// every tick. The DeltaSyncEmitter produces such payloads whenever only
// owner-visible properties were dirty.
TEST(RealEntityData, IsEmptyOtherDelta_TruthTable) {
  // Truly empty — the SerializeOtherDelta didn't even run (hasEvent=false).
  std::vector<std::byte> empty;
  EXPECT_TRUE(RealEntityData::IsEmptyOtherDelta(std::span<const std::byte>(empty)));

  // Flags-only prefix, all zero (uint8 encoding, ≤ 8 replicable props).
  const std::vector<std::byte> zero_u8{std::byte{0}};
  EXPECT_TRUE(RealEntityData::IsEmptyOtherDelta(std::span<const std::byte>(zero_u8)));

  // Flags-only prefix, all zero (uint32 encoding, > 16 props).
  const std::vector<std::byte> zero_u32{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  EXPECT_TRUE(RealEntityData::IsEmptyOtherDelta(std::span<const std::byte>(zero_u32)));

  // Any non-zero byte → real content; must not skip.
  const std::vector<std::byte> flag_set{std::byte{0x01}};
  EXPECT_FALSE(RealEntityData::IsEmptyOtherDelta(std::span<const std::byte>(flag_set)));

  // Flag byte zero but payload non-zero — defensive: should not happen
  // in practice (no flag set ⇒ no field written) but the helper treats
  // it as "has content" to stay on the safe side.
  const std::vector<std::byte> mixed{std::byte{0}, std::byte{0xFF}, std::byte{0xFF}};
  EXPECT_FALSE(RealEntityData::IsEmptyOtherDelta(std::span<const std::byte>(mixed)));
}

// Broadcast decision helper. Per-tick batching means gap > 1 is possible,
// so we need explicit detection instead of firing per-event.
TEST(RealEntityData, ShouldUseSnapshotRefresh_GapTruthTable) {
  // gap == 0: no broadcast needed (caller filters this out); helper
  // still returns false because there's nothing to fall back from.
  EXPECT_FALSE(RealEntityData::ShouldUseSnapshotRefresh(/*latest=*/5, /*last=*/5));
  // gap == 1: BuildDelta covers the single new frame.
  EXPECT_FALSE(RealEntityData::ShouldUseSnapshotRefresh(/*latest=*/6, /*last=*/5));
  // gap > 1: BuildDelta would lose intermediate frames — use snapshot.
  EXPECT_TRUE(RealEntityData::ShouldUseSnapshotRefresh(/*latest=*/7, /*last=*/5));
  EXPECT_TRUE(RealEntityData::ShouldUseSnapshotRefresh(/*latest=*/100, /*last=*/5));
  // Fresh Real (last_broadcast == 0) with first frame (latest == 1):
  // gap == 1, BuildDelta is correct.
  EXPECT_FALSE(RealEntityData::ShouldUseSnapshotRefresh(/*latest=*/1, /*last=*/0));
  // Fresh Real arriving via Offload with pre-existing event_seq: gap
  // jumps from 0 to the carry-over seq in one step → snapshot refresh.
  EXPECT_TRUE(RealEntityData::ShouldUseSnapshotRefresh(/*latest=*/42, /*last=*/0));
}

// ============================================================================
// RealEntityData — velocity
// ============================================================================

TEST(RealEntityData, Velocity_StartsZero_AdvancesByDeltaOverDt) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      40, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto* rd = e->GetRealData();
  EXPECT_FLOAT_EQ(rd->Velocity().x, 0.f);
  rd->UpdateVelocity({0, 0, 0}, 0.1f);  // first sample — still zero.
  EXPECT_FLOAT_EQ(rd->Velocity().x, 0.f);
  rd->UpdateVelocity({10, 0, 0}, 0.1f);  // moved 10 m in 0.1 s → 100 m/s.
  EXPECT_FLOAT_EQ(rd->Velocity().x, 100.f);
}

TEST(RealEntityData, Velocity_DtZeroResetsSampleWithoutCrash) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      41, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto* rd = e->GetRealData();
  rd->UpdateVelocity({1, 0, 1}, 0.f);  // dt=0 -> defensive reset, no div-by-zero.
  EXPECT_FLOAT_EQ(rd->Velocity().x, 0.f);
}

// ============================================================================
// Broadcast seq bookkeeping
// ============================================================================

TEST(RealEntityData, MarkBroadcastSeqs_AreReadBack) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      50, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto* rd = e->GetRealData();
  EXPECT_EQ(rd->LastBroadcastEventSeq(), 0u);
  EXPECT_EQ(rd->LastBroadcastVolatileSeq(), 0u);
  rd->MarkBroadcastEventSeq(11);
  rd->MarkBroadcastVolatileSeq(22);
  EXPECT_EQ(rd->LastBroadcastEventSeq(), 11u);
  EXPECT_EQ(rd->LastBroadcastVolatileSeq(), 22u);
}

// ============================================================================
// GhostApplyDelta — stale / duplicate sequence rejection
// ============================================================================

TEST(RealGhost, GhostApplyDelta_StaleSeqDropped) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 100, uint16_t{1}, space, math::Vector3{0, 0, 0},
      math::Vector3{1, 0, 0}, FakeChannel(0xAA)));
  auto d = MakeBlob({0xAB});
  // Apply at seq=5.
  e->GhostApplyDelta(5, std::span<const std::byte>(d));
  ASSERT_NE(e->GetReplicationState(), nullptr);
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 5u);
  EXPECT_EQ(e->GetReplicationState()->history.size(), 1u);

  // Apply at seq=3 (stale) — must be dropped.
  auto stale = MakeBlob({0xCD});
  e->GhostApplyDelta(3, std::span<const std::byte>(stale));
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 5u);
  EXPECT_EQ(e->GetReplicationState()->history.size(), 1u);
}

TEST(RealGhost, GhostApplyDelta_OutOfOrderDropped) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, 101, uint16_t{1}, space, math::Vector3{0, 0, 0},
      math::Vector3{1, 0, 0}, FakeChannel(0xAA)));
  auto d = MakeBlob({0xEF});
  // Apply at seq=5.
  e->GhostApplyDelta(5, std::span<const std::byte>(d));
  ASSERT_NE(e->GetReplicationState(), nullptr);
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 5u);
  EXPECT_EQ(e->GetReplicationState()->history.size(), 1u);

  // Apply at seq=5 again (duplicate) — must be dropped.
  auto dup = MakeBlob({0x11});
  e->GhostApplyDelta(5, std::span<const std::byte>(dup));
  EXPECT_EQ(e->GetReplicationState()->latest_event_seq, 5u);
  EXPECT_EQ(e->GetReplicationState()->history.size(), 1u);
}

}  // namespace
}  // namespace atlas
