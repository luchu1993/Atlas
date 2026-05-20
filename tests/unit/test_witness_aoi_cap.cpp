#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
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

// ApplyAll resets every option not in the JSON to its compiled default, so
// composite tunings must be applied via a single call.
void ApplyConfigJson(const std::string& body) {
  auto cfg = DataSection::FromJsonString("{" + body + "}");
  ASSERT_TRUE(cfg.HasValue());
  ServerAppOptionBase::ApplyAll(*(*cfg)->Root());
}

// Opt-out of Enter pacing for tests that assert "all pending Enters flushed
// in a single Update" — production paces Enters via witness_enter_bytes_per_tick.
constexpr const char* kNoEnterPacing =
    "\"witness_enter_bytes_per_tick\":67108864,\"witness_max_enters_per_tick\":65535";

void DisableEnterPacing() { ApplyConfigJson(kNoEnterPacing); }

// Per-band caps (close/medium/far). Pass 0 to disable a band entirely.
void SetLodCaps(uint32_t close, uint32_t medium, uint32_t far) {
  ApplyConfigJson(std::string(kNoEnterPacing) +
                  ",\"witness_lod_close_max_peers_per_tick\":" + std::to_string(close) +
                  ",\"witness_lod_medium_max_peers_per_tick\":" + std::to_string(medium) +
                  ",\"witness_lod_far_max_peers_per_tick\":" + std::to_string(far));
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

TEST_F(WitnessAoICapTest, DefaultCapAdmitsEveryEnterUnderForty) {
  DisableEnterPacing();
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

// 20 peers spaced 1m apart all sit in the close band (<25m). With
// close_cap=5 only the 5 closest receive position updates; far/medium
// caps are irrelevant since no peer crosses the 25m threshold.
TEST_F(WitnessAoICapTest, CloseBandCapLimitsPumpToClosestN) {
  SetLodCaps(/*close=*/5, /*medium=*/64, /*far=*/64);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  std::vector<EntityID> ids;
  for (int i = 0; i < 20; ++i) {
    auto* p = MakeEntity(space, 100 + i, {1.f + static_cast<float>(i), 0, 0});
    ids.push_back(p->Id());
    CellEntity::ReplicationFrame v1;
    v1.position = p->Position();
    v1.direction = p->Direction();
    p->PublishReplicationFrame(v1, /*has_event=*/false, /*has_volatile=*/true, {}, {});
  }
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 20u);

  observer->GetWitness()->Update(64 * 1024);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 20u);

  for (auto id : ids) {
    auto* p = space.FindEntity(id);
    CellEntity::ReplicationFrame v2;
    v2.position = p->Position();
    v2.direction = p->Direction();
    p->PublishReplicationFrame(v2, /*has_event=*/false, /*has_volatile=*/true, {}, {});
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

TEST_F(WitnessAoICapTest, AllBandsZeroDisablesPumpButPreservesEnterLeave) {
  SetLodCaps(/*close=*/0, /*medium=*/0, /*far=*/0);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/100.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* peer = MakeEntity(space, 100, {3, 0, 3});
  CellEntity::ReplicationFrame v1;
  v1.position = peer->Position();
  peer->PublishReplicationFrame(v1, /*has_event=*/false, /*has_volatile=*/true, {}, {});

  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityEnter), 1u);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityPositionUpdate), 0u)
      << "zero per-band caps should suppress all pump output";

  ClearSent();
  peer->SetPosition({500.f, 0, 0});
  observer->GetWitness()->Update(4096);
  EXPECT_EQ(CountKind(CellAoIEnvelopeKind::kEntityLeave), 1u);
}

// Three close-band peers (5/15/24m) with close_cap=2: the two closest
// pump first. After the observer walks to x=24, distances rotate and the
// previously-skipped peer becomes one of the two closest.
TEST_F(WitnessAoICapTest, ObserverMovementRotatesCloseBandCappedSet) {
  SetLodCaps(/*close=*/2, /*medium=*/64, /*far=*/64);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* close = MakeEntity(space, 100, {5.f, 0, 0});
  auto* mid = MakeEntity(space, 101, {15.f, 0, 0});
  auto* far = MakeEntity(space, 102, {24.f, 0, 0});
  auto bump = [&]() {
    for (auto* p : {close, mid, far}) {
      CellEntity::ReplicationFrame v;
      v.position = p->Position();
      p->PublishReplicationFrame(v, /*has_event=*/false, /*has_volatile=*/true, {}, {});
    }
  };
  bump();
  observer->GetWitness()->Update(64 * 1024);
  bump();
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
  bump();
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

// Per-band caps mean far peers never compete with close peers. A peer at
// 200m (far band) and a peer at 50m (medium band) both get serviced on
// their own LOD intervals (every 6 / every 3 ticks) regardless of the
// close-band population.
TEST_F(WitnessAoICapTest, FarAndMediumBandsServiceIndependentOfClose) {
  SetLodCaps(/*close=*/1, /*medium=*/1, /*far=*/1);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/500.f, MakeSendFn(), /*hysteresis=*/0.f);

  auto* close = MakeEntity(space, 100, {0, 0, 0});
  auto* mid = MakeEntity(space, 101, {50.f, 0, 0});
  auto* far = MakeEntity(space, 102, {200.f, 0, 0});

  auto bump_volatile = [&]() {
    for (auto* p : {close, mid, far}) {
      CellEntity::ReplicationFrame v;
      v.position = p->Position();
      p->PublishReplicationFrame(v, /*has_event=*/false, /*has_volatile=*/true, {}, {});
    }
  };

  std::set<uint64_t> mid_serviced_at;
  std::set<uint64_t> far_serviced_at;
  for (uint64_t tick = 1; tick <= 30; ++tick) {
    bump_volatile();
    ClearSent();
    observer->GetWitness()->Update(64 * 1024);
    for (const auto& env : sent_) {
      if (KindOf(env) != CellAoIEnvelopeKind::kEntityPositionUpdate) continue;
      const auto id = PublicIdOf(env);
      if (id == mid->Id()) mid_serviced_at.insert(tick);
      if (id == far->Id()) far_serviced_at.insert(tick);
    }
  }

  EXPECT_FALSE(mid_serviced_at.empty())
      << "medium-band peer must be serviced on its own LOD interval";
  EXPECT_FALSE(far_serviced_at.empty()) << "far-band peer must be serviced on its own LOD interval";
}

// Zeroing a single band shuts off updates for that band only — close
// peers keep pumping while far peers stay silent.
TEST_F(WitnessAoICapTest, ZeroFarBandCapSilencesFarOnly) {
  SetLodCaps(/*close=*/64, /*medium=*/64, /*far=*/0);

  Space space(1);
  auto* observer = MakeEntity(space, 1, {0, 0, 0});
  observer->EnableWitness(500.f, MakeSendFn(), 0.f);

  auto* close = MakeEntity(space, 100, {5.f, 0, 0});
  auto* far = MakeEntity(space, 101, {200.f, 0, 0});

  for (uint64_t tick = 1; tick <= 30; ++tick) {
    for (auto* p : {close, far}) {
      CellEntity::ReplicationFrame v;
      v.position = p->Position();
      p->PublishReplicationFrame(v, /*has_event=*/false, /*has_volatile=*/true, {}, {});
    }
    observer->GetWitness()->Update(64 * 1024);
  }

  std::size_t close_updates = 0;
  std::size_t far_updates = 0;
  for (const auto& env : sent_) {
    if (KindOf(env) != CellAoIEnvelopeKind::kEntityPositionUpdate) continue;
    const auto id = PublicIdOf(env);
    if (id == close->Id()) ++close_updates;
    if (id == far->Id()) ++far_updates;
  }
  EXPECT_GT(close_updates, 0u) << "close peer must pump on every tick";
  EXPECT_EQ(far_updates, 0u) << "far_max=0 must silence the far band entirely";
}

}  // namespace
}  // namespace atlas
