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

  void ClearSent() { sent_.clear(); }
};

TEST_F(WitnessAoICapTest, DefaultCapNoTruncationUnderForty) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  for (int i = 0; i < 40; ++i) {
    MakeEntity(space, 100 + i, {1.f + static_cast<float>(i) * 5.f, 0, 0});
  }
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 40u);

  observer->GetWitness()->Update(64 * 1024);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 40u);
}

TEST_F(WitnessAoICapTest, TightCapLimitsPumpToClosestN) {
  SetMaxAoIPeers(5);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

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

  observer->GetWitness()->Update(64 * 1024);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 20u);

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

  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 20u);
}

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

  ClearSent();
  peer->SetPosition({500.f, 0, 0});
  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityLeave), 1u);
}

TEST_F(WitnessAoICapTest, ObserverMovementRotatesCappedSet) {
  SetMaxAoIPeers(2);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
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

TEST_F(WitnessAoICapTest, FarPeerForceServicedAfterStarvationThreshold) {
  SetCapAndStarvation(/*cap=*/1, /*threshold=*/5);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* close = MakeEntity(space, 100, {0, 0, 0});
  auto* mid = MakeEntity(space, 101, {50.f, 0, 0});
  auto* far = MakeEntity(space, 102, {200.f, 0, 0});

  auto bump_volatile = [&](uint64_t seq) {
    for (auto* p : {close, mid, far}) {
      CellEntity::ReplicationFrame v;
      v.volatile_seq = seq;
      v.position = p->Position();
      p->PublishReplicationFrame(v, {}, {});
    }
  };

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
