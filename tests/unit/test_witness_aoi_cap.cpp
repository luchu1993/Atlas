// Witness AoI soft-cap tests.
//
// witness_max_aoi_peers limits how many peers compete for the per-tick
// pump's service slots. Far peers stay tracked in aoi_map_ (so Enter /
// Leave / snapshot membership stays correct) but skip the pump.
//
// Tests cover:
//   - Default cap (50) doesn't truncate when peer count is well below.
//   - Tight cap (e.g. 5) limits per-tick pumped peers and prefers the
//     closest by squared distance.
//   - aoi_map_ size is unaffected by the cap (membership = spatial range).
//   - Far-peer Leave still fires regardless of cap (Leaves drain in
//     Transitions, before the heap step).
//   - Observer movement re-ranks peers; previously-skipped peers can
//     enter the top-N by walking closer.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "cellapp_config.h"
#include "math/vector3.h"
#include "serialization/data_section.h"
#include "server/server_app_option.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

struct CapturedEnvelope {
  std::vector<std::byte> payload;
};

auto KindOf(const CapturedEnvelope& e) -> CellAoIEnvelopeKind {
  return static_cast<CellAoIEnvelopeKind>(e.payload.at(0));
}

auto PublicIdOf(const CapturedEnvelope& e) -> EntityID {
  EntityID id = 0;
  for (int i = 0; i < 4; ++i) {
    id |= static_cast<EntityID>(e.payload[1 + i]) << (i * 8);
  }
  return id;
}

void ResetCellAppConfigToDefaults() {
  auto empty = DataSection::FromJsonString("{}");
  ASSERT_TRUE(empty.HasValue());
  ServerAppOptionBase::ApplyAll(*(*empty)->Root());
}

void SetMaxAoIPeers(uint32_t cap) {
  const auto json = std::string("{\"witness_max_aoi_peers\":") + std::to_string(cap) + "}";
  auto cfg = DataSection::FromJsonString(json);
  ASSERT_TRUE(cfg.HasValue());
  ServerAppOptionBase::ApplyAll(*(*cfg)->Root());
}

void SetCapAndStarvation(uint32_t cap, uint32_t threshold) {
  const auto json = std::string("{\"witness_max_aoi_peers\":") + std::to_string(cap) +
                    ",\"witness_starvation_threshold_ticks\":" + std::to_string(threshold) + "}";
  auto cfg = DataSection::FromJsonString(json);
  ASSERT_TRUE(cfg.HasValue());
  ServerAppOptionBase::ApplyAll(*(*cfg)->Root());
}

class WitnessAoICapTest : public ::testing::Test {
 protected:
  std::vector<CapturedEnvelope> sent_;

  void SetUp() override { ResetCellAppConfigToDefaults(); }
  void TearDown() override { ResetCellAppConfigToDefaults(); }

  auto MakeSendFn() {
    return [this](std::span<const std::byte> env) {
      sent_.push_back({std::vector<std::byte>(env.begin(), env.end())});
    };
  }

  static auto MakeEntity(Space& space, EntityID id, math::Vector3 pos) -> CellEntity* {
    auto* e = space.AddEntity(std::make_unique<CellEntity>(id, /*type_id=*/uint16_t{1}, space, pos,
                                                           math::Vector3{1, 0, 0}));
    e->SetBaseAddr(Address(0, 0));
    return e;
  }

  auto CountKind(CellAoIEnvelopeKind kind) const -> std::size_t {
    return static_cast<std::size_t>(
        std::count_if(sent_.begin(), sent_.end(),
                      [kind](const CapturedEnvelope& e) { return KindOf(e) == kind; }));
  }

  // Drain Enter envelopes between phases so each tick's Pump output is
  // distinguishable.
  void ClearSent() { sent_.clear(); }
};

// ----------------------------------------------------------------------------
// Default cap (50) does not truncate when peer count <= cap.
// ----------------------------------------------------------------------------

TEST_F(WitnessAoICapTest, DefaultCapNoTruncationUnderForty) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  // 40 peers at increasing distances along x-axis, all within AoI radius.
  // Default cap is 50, so all 40 should compete for service slots.
  for (int i = 0; i < 40; ++i) {
    MakeEntity(space, 100 + i, {1.f + i * 5.f, 0, 0});
  }
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 40u);

  // First tick drains pending_enter → 40 EntityEnter envelopes regardless
  // of cap (Enter is in the Transitions step, not the heap step).
  observer->GetWitness()->Update(64 * 1024);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 40u);
}

// ----------------------------------------------------------------------------
// Tight cap limits per-tick pumped peers; closest-N preferred.
// ----------------------------------------------------------------------------

TEST_F(WitnessAoICapTest, TightCapLimitsPumpToClosestN) {
  SetMaxAoIPeers(5);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  // 20 peers, ids 100..119, all in close LOD band so a single LOD
  // interval (1 tick) doesn't mask the cap effect. Distances 1m..20m.
  std::vector<EntityID> ids;
  for (int i = 0; i < 20; ++i) {
    auto* p = MakeEntity(space, 100 + i, {1.f + static_cast<float>(i), 0, 0});
    ids.push_back(p->Id());
    CellEntity::ReplicationFrame v1;
    v1.volatile_seq = 1;
    v1.position = p->Position();
    v1.direction = p->Direction();
    p->PublishReplicationFrame(v1, {}, {});
  }
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 20u);

  // Tick 1: drains 20 Enters (which absorb volatile_seq=1), so Pump has
  // no follow-up volatile to send this tick.
  observer->GetWitness()->Update(64 * 1024);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 20u);

  // Bump volatile_seq=2 on every peer so Pump now has work; cap should
  // limit it to the closest 5.
  for (int i = 0; i < 20; ++i) {
    auto* p = space.FindEntity(ids[i]);
    ASSERT_NE(p, nullptr);
    CellEntity::ReplicationFrame v2;
    v2.volatile_seq = 2;
    v2.position = p->Position();
    v2.direction = p->Direction();
    p->PublishReplicationFrame(v2, {}, {});
  }
  ClearSent();

  // Tick 2: PriorityHeap caps to 5; Pump services exactly those.
  observer->GetWitness()->Update(64 * 1024);

  std::set<EntityID> updated;
  for (const auto& env : sent_) {
    if (KindOf(env) == CellAoIEnvelopeKind::kEntityPositionUpdate) {
      updated.insert(PublicIdOf(env));
    }
  }
  EXPECT_EQ(updated.size(), 5u);
  for (EntityID expected : {ids[0], ids[1], ids[2], ids[3], ids[4]}) {
    EXPECT_EQ(updated.count(expected), 1u)
        << "peer " << expected << " (closest tier) should have been pumped";
  }

  // aoi_map_ unaffected by the cap - membership tracks the spatial
  // trigger, not the rank cut.
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 20u);
}

// ----------------------------------------------------------------------------
// Cap == 0 leaves the priority queue empty: Pump emits no PositionUpdates.
// Enter / Leave still flow via Transitions.
// ----------------------------------------------------------------------------

TEST_F(WitnessAoICapTest, ZeroCapDisablesPumpButPreservesEnterLeave) {
  SetMaxAoIPeers(0);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/100.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* peer = MakeEntity(space, 100, {3, 0, 3});
  CellEntity::ReplicationFrame v1;
  v1.volatile_seq = 1;
  v1.position = peer->Position();
  peer->PublishReplicationFrame(v1, {}, {});

  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 1u);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityPositionUpdate), 0u)
      << "cap=0 should suppress all pump output";

  // Move peer outside AoI - the Leave path is independent of the cap.
  ClearSent();
  peer->SetPosition({500.f, 0, 0});
  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityLeave), 1u);
}

// ----------------------------------------------------------------------------
// A peer skipped due to cap can re-enter the top-N by becoming closer
// (observer walks toward it). Validates the lazy / per-tick rebalancing.
// ----------------------------------------------------------------------------

TEST_F(WitnessAoICapTest, ObserverMovementRotatesCappedSet) {
  SetMaxAoIPeers(2);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  // Cap=2, anti-starvation off so the test asserts strictly on rank-cut
  // behaviour. Three peers placed at 5/15/24 m - all in the close LOD
  // band so per-tick eligibility doesn't muddy the assertion.
  SetCapAndStarvation(/*cap=*/2, /*threshold=*/0);

  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* close = MakeEntity(space, 100, {5.f, 0, 0});
  auto* mid = MakeEntity(space, 101, {15.f, 0, 0});
  auto* far = MakeEntity(space, 102, {24.f, 0, 0});
  auto bump = [&](uint64_t seq) {
    for (auto* p : {close, mid, far}) {
      CellEntity::ReplicationFrame v;
      v.volatile_seq = seq;
      v.position = p->Position();
      p->PublishReplicationFrame(v, {}, {});
    }
  };
  bump(1);

  // Tick 1 absorbs Enters; Tick 2 (after volatile bump) is the first
  // tick whose Pump produces PositionUpdates.
  observer->GetWitness()->Update(64 * 1024);
  bump(2);
  ClearSent();
  observer->GetWitness()->Update(64 * 1024);

  std::set<EntityID> phase1_updated;
  for (const auto& env : sent_) {
    if (KindOf(env) == CellAoIEnvelopeKind::kEntityPositionUpdate) {
      phase1_updated.insert(PublicIdOf(env));
    }
  }
  EXPECT_EQ(phase1_updated.count(close->Id()), 1u);
  EXPECT_EQ(phase1_updated.count(mid->Id()), 1u);
  EXPECT_EQ(phase1_updated.count(far->Id()), 0u) << "far is rank-cut while observer is at origin";

  // Phase 2: observer walks to (24,0,0). New squared distances:
  //   close (5,0,0)  -> 19^2 = 361
  //   mid   (15,0,0) ->  9^2 = 81
  //   far   (24,0,0) ->  0^2 = 0
  // Closest 2 = far + mid; close drops out of the rank cut.
  observer->SetPosition({24.f, 0, 0});
  bump(3);
  ClearSent();
  observer->GetWitness()->Update(64 * 1024);

  std::set<EntityID> phase2_updated;
  for (const auto& env : sent_) {
    if (KindOf(env) == CellAoIEnvelopeKind::kEntityPositionUpdate) {
      phase2_updated.insert(PublicIdOf(env));
    }
  }
  EXPECT_EQ(phase2_updated.count(far->Id()), 1u)
      << "far becomes the closest after observer walked - must be pumped";
  EXPECT_EQ(phase2_updated.count(mid->Id()), 1u);
  EXPECT_EQ(phase2_updated.count(close->Id()), 0u)
      << "close drops out of the cap once observer moves away";
}

// ----------------------------------------------------------------------------
// Anti-starvation: a chronically far peer in a dense close band gets
// force-serviced once its skip count exceeds the threshold.
// ----------------------------------------------------------------------------

TEST_F(WitnessAoICapTest, FarPeerForceServicedAfterStarvationThreshold) {
  // cap=1 keeps only the closest peer per tick. With three peers placed
  // at distance 5 / 50 / 200 the closest gets serviced every tick; the
  // mid and far peers would starve forever without the backstop.
  // Threshold=5 forces them to surface within ~5 ticks of starvation.
  SetCapAndStarvation(/*cap=*/1, /*threshold=*/5);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* close = MakeEntity(space, 100, {5.f, 0, 0});
  auto* mid = MakeEntity(space, 101, {50.f, 0, 0});
  auto* far = MakeEntity(space, 102, {200.f, 0, 0});

  // Bump volatile_seq each tick so every peer is "ready to be pumped"
  // every tick (no LOD skip masks the starvation behaviour).
  auto bump_volatile = [&](uint64_t seq) {
    for (auto* p : {close, mid, far}) {
      CellEntity::ReplicationFrame v;
      v.volatile_seq = seq;
      v.position = p->Position();
      p->PublishReplicationFrame(v, {}, {});
    }
  };

  // Drive enough ticks that mid and far would each be skipped past the
  // starvation threshold without the backstop. With threshold=5 we
  // expect each to be serviced at least once within that window.
  std::set<uint64_t> mid_serviced_at;
  std::set<uint64_t> far_serviced_at;
  for (uint64_t tick = 1; tick <= 30; ++tick) {
    bump_volatile(tick);
    ClearSent();
    observer->GetWitness()->Update(64 * 1024);
    for (const auto& env : sent_) {
      if (KindOf(env) != CellAoIEnvelopeKind::kEntityPositionUpdate) continue;
      const auto id = PublicIdOf(env);
      if (id == mid->Id()) mid_serviced_at.insert(tick);
      if (id == far->Id()) far_serviced_at.insert(tick);
    }
  }

  EXPECT_FALSE(mid_serviced_at.empty()) << "mid peer should have been force-serviced "
                                           "at least once within 30 ticks";
  EXPECT_FALSE(far_serviced_at.empty()) << "far peer should have been force-serviced "
                                           "at least once within 30 ticks";
}

// Disabling the backstop (threshold=0) reverts to pure rank-cut behaviour:
// far peers in a dense close band do not surface within the same window.
TEST_F(WitnessAoICapTest, ZeroThresholdDisablesAntiStarvation) {
  SetCapAndStarvation(/*cap=*/1, /*threshold=*/0);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(500.f, MakeSendFn(), 0.f);

  auto* close = MakeEntity(space, 100, {5.f, 0, 0});
  auto* far = MakeEntity(space, 101, {200.f, 0, 0});

  for (uint64_t tick = 1; tick <= 30; ++tick) {
    for (auto* p : {close, far}) {
      CellEntity::ReplicationFrame v;
      v.volatile_seq = tick;
      v.position = p->Position();
      p->PublishReplicationFrame(v, {}, {});
    }
    observer->GetWitness()->Update(64 * 1024);
  }

  std::size_t far_count = 0;
  for (const auto& env : sent_) {
    if (KindOf(env) == CellAoIEnvelopeKind::kEntityPositionUpdate && PublicIdOf(env) == far->Id()) {
      ++far_count;
    }
  }
  EXPECT_EQ(far_count, 0u)
      << "with threshold=0 the far peer should remain skipped behind the closer one";
}

}  // namespace
}  // namespace atlas
