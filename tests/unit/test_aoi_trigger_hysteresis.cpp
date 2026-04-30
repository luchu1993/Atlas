// Hysteresis (double-band) AoITrigger tests. Cover:
//   - A peer inside the hysteresis window (between inner and outer
//     radii) does NOT fire EntityLeave.
//   - A peer oscillating across the inner boundary produces a single
//     EntityEnter + zero EntityLeaves (the "boundary thrash" bug that
//     single-band triggers suffer).
//   - A peer crossing the outer boundary fires EntityLeave.
//   - hysteresis = 0 degenerates to single-band behaviour.
//   - SetAoIRadius(new_r, new_hyst) reshapes both bands in place.

#include <cstddef>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

struct CapturedEnvelope {
  EntityID observer_base_id;
  std::vector<std::byte> payload;
};

class AoIHysteresisTest : public ::testing::Test {
 protected:
  std::vector<CapturedEnvelope> sent_;

  auto MakeSendFn() {
    return [this](EntityID observer_id, std::span<const std::byte> env) {
      sent_.push_back({observer_id, std::vector<std::byte>(env.begin(), env.end())});
    };
  }

  static auto KindOf(const CapturedEnvelope& e) -> CellAoIEnvelopeKind {
    return static_cast<CellAoIEnvelopeKind>(e.payload.at(0));
  }

  static auto MakeEntity(Space& space, EntityID id, uint16_t type_id, math::Vector3 pos)
      -> CellEntity* {
    auto* e = space.AddEntity(
        std::make_unique<CellEntity>(id, type_id, space, pos, math::Vector3{1, 0, 0}));
    e->SetBaseAddr(Address(0, 0));
    return e;
  }

  // Count envelopes of a given kind (across Enter / Leave / Update) that
  // were emitted since the last `sent_.clear()`.
  [[nodiscard]] auto CountOf(CellAoIEnvelopeKind kind) const -> std::size_t {
    std::size_t n = 0;
    for (const auto& e : sent_) {
      if (KindOf(e) == kind) ++n;
    }
    return n;
  }
};

// Peer at distance 12 sits between inner r=10 and outer r=15 — in the
// hysteresis window. Having first entered AoI (via a fresh spawn inside
// inner), moving into the hysteresis window must NOT fire a Leave.
TEST_F(AoIHysteresisTest, PeerInHysteresisWindowDoesNotLeave) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(/*aoi_radius=*/10.f, MakeSendFn(), /*hysteresis=*/5.f);

  auto* peer = MakeEntity(space, 100, 7, {3.f, 0.f, 0.f});  // inside inner
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(CountOf(CellAoIEnvelopeKind::kEntityEnter), 1u);
  sent_.clear();

  // Move peer into the hysteresis window (outside inner 10, inside outer 15).
  peer->SetPosition({12.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(CountOf(CellAoIEnvelopeKind::kEntityLeave), 0u)
      << "peer in hysteresis band must stay in AoI";
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
}

// Oscillation test: peer moves across the inner boundary several times
// while staying inside the outer band. Single-band AoI would produce
// enter/leave/enter/leave flap; hysteresis should produce exactly one
// enter and zero leaves.
TEST_F(AoIHysteresisTest, HysteresisPreventsBoundaryThrash) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn(), 5.f);

  // Spawn peer just inside inner radius so first Update fires a single Enter.
  auto* peer = MakeEntity(space, 100, 7, {9.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(CountOf(CellAoIEnvelopeKind::kEntityEnter), 1u);
  sent_.clear();

  // Oscillate across the inner boundary (10) but never leave the outer
  // band (15). Single-band would produce 4 × (enter, leave) pairs; dual-
  // band must be silent.
  const float positions[] = {11.f, 9.f, 11.f, 9.f, 11.f, 9.f, 11.f, 9.f};
  for (float x : positions) {
    peer->SetPosition({x, 0.f, 0.f});
    observer->GetWitness()->Update(4096);
  }

  EXPECT_EQ(CountOf(CellAoIEnvelopeKind::kEntityEnter), 0u);
  EXPECT_EQ(CountOf(CellAoIEnvelopeKind::kEntityLeave), 0u);
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
}

// Crossing the outer boundary genuinely leaves AoI.
TEST_F(AoIHysteresisTest, CrossingOuterBoundaryFiresLeave) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn(), 5.f);

  auto* peer = MakeEntity(space, 100, 7, {3.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  sent_.clear();

  // Move peer well outside the outer band.
  peer->SetPosition({100.f, 0.f, 100.f});
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(CountOf(CellAoIEnvelopeKind::kEntityLeave), 1u);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

// hysteresis = 0 collapses the outer band onto the inner. Crossing outward
// fires Leave immediately — the single-band fallback behaviour.
TEST_F(AoIHysteresisTest, ZeroHysteresisBehavesAsSingleBand) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn(), 0.f);

  auto* peer = MakeEntity(space, 100, 7, {3.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);
  sent_.clear();

  // Move peer just outside the (single) boundary.
  peer->SetPosition({12.f, 0.f, 0.f});
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(CountOf(CellAoIEnvelopeKind::kEntityLeave), 1u);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

// SetAoIRadius resizes both bands in place. A peer inside the new
// hysteresis window stays in AoI; a peer outside the new outer leaves.
TEST_F(AoIHysteresisTest, SetAoIRadiusReshapesBothBands) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(50.f, MakeSendFn(), 10.f);  // inner 50, outer 60

  auto* peer_in_hyst = MakeEntity(space, 100, 7, {12.f, 0.f, 0.f});  // inside old inner
  auto* peer_far = MakeEntity(space, 200, 7, {17.f, 0.f, 0.f});      // inside old inner
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 2u);
  sent_.clear();

  // Shrink: new inner 10, new outer 15.
  //   - peer_in_hyst at 12: outside new inner but inside new outer → stays
  //   - peer_far at 17: outside both new inner and new outer → leaves
  observer->GetWitness()->SetAoIRadius(10.f, 5.f);
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(CountOf(CellAoIEnvelopeKind::kEntityLeave), 1u);
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().count(peer_in_hyst->Id()) == 1);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().count(peer_far->Id()) == 0);
}

// Witness::Hysteresis() accessor surfaces the configured band.
TEST_F(AoIHysteresisTest, HysteresisAccessorSurfacesConfiguredBand) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn(), 5.f);
  EXPECT_FLOAT_EQ(observer->GetWitness()->AoIRadius(), 10.f);
  EXPECT_FLOAT_EQ(observer->GetWitness()->Hysteresis(), 5.f);

  observer->GetWitness()->SetAoIRadius(30.f, 2.f);
  EXPECT_FLOAT_EQ(observer->GetWitness()->AoIRadius(), 30.f);
  EXPECT_FLOAT_EQ(observer->GetWitness()->Hysteresis(), 2.f);
}

}  // namespace
}  // namespace atlas
