// Witness peer-lifetime tests — review follow-up.
//
// When a peer CellEntity is destroyed while still inside another
// entity's Witness AoI, does the observer's aoi_map_ dangle? The
// RangeList::Remove path doesn't fire OnLeave events, so any trigger
// that had the peer in its inside_peers_ set retains a dangling
// pointer. This test exposes the bug: observer.GetWitness()->Update()
// after peer destruction would UAF the dead entity's GetReplicationState.

#include <memory>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

class WitnessPeerLifetimeTest : public ::testing::Test {
 protected:
  Space space_{1};

  auto MakeEntity(EntityID id, math::Vector3 pos = {0, 0, 0}) -> CellEntity* {
    auto* e =
        space_.AddEntity(std::make_unique<CellEntity>(id, 1, space_, pos, math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), id + 1000);
    return e;
  }
};

// Two separate things must happen when a peer is destroyed while
// still inside the observer's AoI:
//   (a) the next Update fires an EntityLeave envelope (no missing leaves)
//   (b) the witness must NOT dereference the freed peer during that
//       Update — we cache the peer's base_entity_id at enter time so
//       SendEntityLeave has its own copy to work with.
// This test exercises both. Without (b) in place, the Update below
// would UAF on cache.entity->BaseEntityId() — in debug builds it
// usually crashes or produces garbage base ids; ASan would catch it.
TEST_F(WitnessPeerLifetimeTest, PeerDestructionTriggersLeaveEventAndCleansCache) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  auto* peer = MakeEntity(2, {3, 0, 3});

  int leave_count = 0;
  observer->EnableWitness(10.f, [&](EntityID, std::span<const std::byte> env) {
    // Kind == EntityLeave (2) → counts as a leave event.
    if (!env.empty() && static_cast<uint8_t>(env[0]) == 2) ++leave_count;
  });

  // Tick once so the initial EntityEnter is flushed; cache is now in
  // "normal" (non-EnterPending) state.
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  // Destroy the peer. Space::RemoveEntity destructs the CellEntity
  // synchronously; the synthetic FLT_MAX shuffle in ~CellEntity fires
  // OnLeave on the observer's trigger, which marks the cache kGone.
  // Everything up to this point works with a live peer.
  space_.RemoveEntity(peer->Id());

  // Now the peer is freed. The next Update iterates gone_ids and must
  // NOT dereference cache.entity — it has to use the base_id cached at
  // enter time.
  observer->GetWitness()->Update(4096);

  ASSERT_EQ(leave_count, 1) << "peer destruction should produce exactly one OnLeave";
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty())
      << "dead peer must be compacted out of aoi_map_";
}

// Dedicated assertion that the EntityLeave envelope carries the peer's
// base_entity_id — if Witness dereferences the freed CellEntity to get
// it, ASan or a read of garbage memory would produce a mismatching id
// with high probability.
TEST_F(WitnessPeerLifetimeTest, LeaveEnvelopeCarriesCorrectBaseIdAfterPeerFreed) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  auto* peer = MakeEntity(2, {3, 0, 3});
  const EntityID expected_base = peer->BaseEntityId();

  EntityID last_leave_base = 0;
  observer->EnableWitness(10.f, [&](EntityID, std::span<const std::byte> env) {
    if (env.size() >= 5 && static_cast<uint8_t>(env[0]) == 2) {
      // Decode base_id: bytes [1..4] little-endian.
      last_leave_base = static_cast<EntityID>(env[1]) | (static_cast<EntityID>(env[2]) << 8) |
                        (static_cast<EntityID>(env[3]) << 16) |
                        (static_cast<EntityID>(env[4]) << 24);
    }
  });
  observer->GetWitness()->Update(4096);  // flush enter

  space_.RemoveEntity(peer->Id());
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(last_leave_base, expected_base)
      << "Leave envelope must carry the base_id captured at enter time, "
         "not a read-through of a freed CellEntity";
}

}  // namespace
}  // namespace atlas
