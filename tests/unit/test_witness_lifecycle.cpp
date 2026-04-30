// Witness + AoITrigger lifecycle tests.
//
// Tests cover:
//   - AoI radius driving Enter / Leave events through the trigger layer
//   - EntityEnter / EntityLeave envelope emission through the reliable
//     delivery callback
//   - SetAoIRadius growing / shrinking the trigger in place
//   - Witness teardown removing the trigger bounds cleanly
//
// This file exercises only the state-machine skeleton (ENTER_PENDING,
// GONE, REFRESH transitions); ordered-property-delta and volatile-position
// catch-up logic live in test_witness_replication.

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

class WitnessLifecycleTest : public ::testing::Test {
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
  static auto PublicIdOf(const CapturedEnvelope& e) -> EntityID {
    EntityID id = 0;
    for (int i = 0; i < 4; ++i) {
      id |= static_cast<EntityID>(e.payload[1 + i]) << (i * 8);
    }
    return id;
  }

  static auto MakeEntity(Space& space, EntityID id, uint16_t type_id, math::Vector3 pos)
      -> CellEntity* {
    auto* e = space.AddEntity(
        std::make_unique<CellEntity>(id, type_id, space, pos, math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), /*base_id=*/id);
    return e;
  }
};

TEST_F(WitnessLifecycleTest, NoPeersNoEvents) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(/*radius=*/10.f, MakeSendFn());
  observer->GetWitness()->Update(/*max_packet_bytes=*/4096);

  EXPECT_TRUE(sent_.empty());
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

TEST_F(WitnessLifecycleTest, PeerEntersFiresEntityEnter) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn());

  // Peer spawns inside AoI → trigger fires HandleAoIEnter on the
  // witness, which caches the peer as ENTER_PENDING.
  auto* peer = MakeEntity(space, 100, 7, {3, 0, 3});
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
  ASSERT_TRUE(sent_.empty()) << "Enter events are deferred to Update, not the trigger callback";

  observer->GetWitness()->Update(4096);

  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityEnter);
  EXPECT_EQ(PublicIdOf(sent_[0]), peer->BaseEntityId());
  EXPECT_EQ(sent_[0].observer_base_id, observer->BaseEntityId());

  // Follow-up Update with no state change produces nothing.
  sent_.clear();
  observer->GetWitness()->Update(4096);
  EXPECT_TRUE(sent_.empty());
}

TEST_F(WitnessLifecycleTest, PeerLeavesFiresEntityLeave) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn());
  auto* peer = MakeEntity(space, 100, 7, {3, 0, 3});

  observer->GetWitness()->Update(4096);
  sent_.clear();

  // Move peer out of AoI.
  peer->SetPosition({100.f, 0.f, 100.f});

  // Trigger fires HandleAoILeave → cache flagged GONE. Envelope not
  // sent until Update consumes the flag.
  ASSERT_TRUE(sent_.empty());

  observer->GetWitness()->Update(4096);

  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityLeave);
  EXPECT_EQ(PublicIdOf(sent_[0]), peer->BaseEntityId());
  // After Update, the cache is compacted.
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

TEST_F(WitnessLifecycleTest, SetAoIRadiusExpandSweepsInPeer) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(5.f, MakeSendFn());

  // Peer outside initial r=5 AoI.
  auto* peer = MakeEntity(space, 100, 7, {20, 0, 0});
  observer->GetWitness()->Update(4096);
  ASSERT_TRUE(observer->GetWitness()->AoIMap().empty());
  sent_.clear();

  // Expand radius; peer falls inside.
  observer->GetWitness()->SetAoIRadius(50.f);
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  observer->GetWitness()->Update(4096);
  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityEnter);
  EXPECT_EQ(PublicIdOf(sent_[0]), peer->BaseEntityId());
}

TEST_F(WitnessLifecycleTest, DeactivateClearsState) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn());
  MakeEntity(space, 100, 7, {3, 0, 3});
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  observer->GetWitness()->Deactivate();
  EXPECT_FALSE(observer->GetWitness()->IsActive());
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

TEST_F(WitnessLifecycleTest, WitnessTeardownOnEntityDestroy) {
  // If a witness owner is destroyed while the trigger is still active,
  // the CellEntity destructor must tear the trigger down before
  // unlinking the range node. Otherwise the AoITrigger bound nodes
  // would outlive the RangeList and dangle.
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(10.f, MakeSendFn());
  MakeEntity(space, 100, 7, {3, 0, 3});  // peer present so trigger has state

  // Tick once to flush the initial Enter.
  observer->GetWitness()->Update(4096);

  // Destroy the observer — destructor should run without hitting any
  // assertion and without leaving dangling nodes in the RangeList.
  space.RemoveEntity(1);
  EXPECT_EQ(space.EntityCount(), 1u);  // only the peer remains
}

// Peers filtered by the trigger's own 2-D bounds — an AoI of radius 5
// around origin is actually a square of side 10; a peer at (20, 0) is
// well outside and must never make it into the aoi_map_.
TEST_F(WitnessLifecycleTest, PeerOutsideRadiusNeverEnters) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1, {0, 0, 0});
  observer->EnableWitness(5.f, MakeSendFn());
  MakeEntity(space, 100, 7, {20, 0, 20});

  observer->GetWitness()->Update(4096);
  EXPECT_TRUE(sent_.empty());
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

}  // namespace
}  // namespace atlas
