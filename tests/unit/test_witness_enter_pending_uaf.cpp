// ASan-targeted reproducer for the cellapp profile-build crash.
//
// Live crash signature (from a profile-run dump):
//   atlas::Witness::SendEntityEnter +0x10c   witness.cc:236
//   atlas::Witness::Update         +0x32a   witness.cc:295
// `cache.entity` was a non-null pointer into freed heap whose vtable slot
// had already been recycled into floating-point data — a textbook
// use-after-free of the peer CellEntity.
//
// Hypothesis: when a peer enters an observer's AoI but is destroyed
// before the next Witness::Update, the destruction path is supposed to
// fire HandleAoILeave (which nulls cache.entity).  If the destruction
// path can sidestep the leave callback for any reason, cache.entity
// stays dangling and SendEntityEnter walks freed memory.
//
// Each test below exercises one variant of "enter and then immediately
// destroy."  Built with /fsanitize=address (preset asan-msvc) and run
// in isolation — the second one trips ASan iff there is still a path
// where ~CellEntity bypasses the leave fan-out.

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

class WitnessEnterPendingUafTest : public ::testing::Test {
 protected:
  Space space_{1};

  CellEntity* MakeEntity(EntityID id, math::Vector3 pos = {0, 0, 0}) {
    auto* e = space_.AddEntity(
        std::make_unique<CellEntity>(id, uint16_t{1}, space_, pos, math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), id);
    return e;
  }
};

// Baseline: Enter then Update on a live peer must succeed.
TEST_F(WitnessEnterPendingUafTest, EnterPendingFlushedNormally) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  /* peer = */ MakeEntity(2, {3, 0, 3});
  observer->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {});

  // Observer's witness was activated by EnableWitness; peer should be
  // inside on first Update.  No destruction in between.
  observer->GetWitness()->Update(4096);
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
}

// The repro: peer enters AoI (cache becomes kEnterPending), then is
// destroyed BEFORE the observer's first Update.  Update must not
// dereference cache.entity — the synthetic FLT_MAX shuffle in
// ~CellEntity is responsible for nulling cache.entity via
// HandleAoILeave.  ASan would catch a UAF here if the contract slips.
TEST_F(WitnessEnterPendingUafTest, PeerDestroyedWhileEnterPending) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  auto* peer = MakeEntity(2, {3, 0, 3});

  int leave_count = 0;
  int enter_count = 0;
  observer->EnableWitness(10.f, [&](EntityID, std::span<const std::byte> env) {
    if (env.empty()) return;
    const auto kind = static_cast<uint8_t>(env[0]);
    if (kind == 1) ++enter_count;  // kEntityEnter
    if (kind == 2) ++leave_count;  // kEntityLeave
  });

  // EnableWitness inserted the trigger and inner-band's initial scan
  // populated aoi_map_ with the peer in kEnterPending state.  Confirm
  // we are in the dangerous state, i.e. an entry exists but no Enter
  // envelope has been flushed yet.
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
  ASSERT_EQ(enter_count, 0);

  // Destroy the peer.  Space::RemoveEntity → ~CellEntity → synthetic
  // FLT_MAX shuffle → OuterTrigger.OnLeave → Witness::HandleAoILeave
  // (which is supposed to clear kEnterPending and null cache.entity).
  space_.RemoveEntity(peer->Id());

  // If HandleAoILeave fired, the cache is in kGone state and the next
  // Update fires an EntityLeave (or compacts silently if it never even
  // sent an Enter).  If HandleAoILeave was missed, Update derefs the
  // freed peer in SendEntityEnter — ASan flags heap-use-after-free.
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(enter_count, 0) << "no Enter should be sent for a peer that died first";
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty()) << "dead peer must compact out";
}

// Stress variant: many peers transition together.  ASan often only
// catches a bug when multiple frees happen in close succession (so the
// freed slot gets reused before iteration reaches it).
TEST_F(WitnessEnterPendingUafTest, ManyPeersDestroyedWhileEnterPending) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  std::vector<EntityID> peer_ids;
  for (int i = 0; i < 32; ++i) {
    const EntityID id = 100 + i;
    MakeEntity(id, {static_cast<float>(i % 8), 0, static_cast<float>(i / 8)});
    peer_ids.push_back(id);
  }

  observer->EnableWitness(20.f, [](EntityID, std::span<const std::byte>) {});
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), peer_ids.size());

  // Tear them all down before the first Update.
  for (auto id : peer_ids) space_.RemoveEntity(id);

  observer->GetWitness()->Update(64 * 1024);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty());
}

// Re-entrant destruction: the witness's send callback itself destroys
// peers.  This mimics what happens when the live cellapp's
// `send_reliable_` triggers a packet failure that causes a peer's
// channel to be torn down, which in turn destroys an entity.  If
// HandleAoILeave doesn't get a chance to run because the destruction
// path is asynchronous wrt the iteration, the next Update would walk
// freed memory.
TEST_F(WitnessEnterPendingUafTest, SendCallbackDestroysAnotherPeer) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  std::vector<EntityID> peer_ids;
  for (int i = 0; i < 8; ++i) {
    const EntityID id = 200 + i;
    MakeEntity(id, {1.f + i * 0.5f, 0, 0.5f});
    peer_ids.push_back(id);
  }

  // Send callback destroys the *next* peer mid-iteration.  This forces
  // reuse of the freed cache.entity slot (if any path leaves it
  // dangling) within the same Update pass.
  std::size_t send_count = 0;
  observer->EnableWitness(20.f, [&](EntityID, std::span<const std::byte>) {
    if (send_count < peer_ids.size() - 1) {
      space_.RemoveEntity(peer_ids[send_count + 1]);
    }
    ++send_count;
  });

  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), peer_ids.size());

  observer->GetWitness()->Update(64 * 1024);
  // The first peer's Enter fires; that callback destroys peer #2; on
  // peer #2's iteration cache.entity must already be null'd.
  observer->GetWitness()->Update(64 * 1024);  // drain leaves too
}

// Defensive: cache.entity points at memory that is not in space.entities_.
// In the live cellapp this happens via at least one not-yet-identified
// destruction path that bypasses HandleAoILeave.  We simulate it by
// poking the cache directly.  The Update loop must catch the stale
// pointer (via space.FindEntity()) and skip rather than UAF.
TEST_F(WitnessEnterPendingUafTest, StaleCacheDanglingPointerSurvives) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  auto* peer = MakeEntity(2, {3, 0, 3});

  observer->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {});
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  // Smuggle a dangling pointer into the cache without going through
  // HandleAoILeave — this is what some live cellapp path manages to do
  // by accident.  We point at a fake non-null address that doesn't
  // correspond to any entity in space.entities_.
  auto& mutable_cache =
      const_cast<Witness::EntityCache&>(observer->GetWitness()->AoIMap().find(peer->Id())->second);
  uintptr_t fake = 0xdeadbeefULL;
  mutable_cache.entity = reinterpret_cast<CellEntity*>(fake);

  // Real peer is removed too — Space::FindEntity will return nullptr
  // for this id, exercising the live != cache.entity branch.
  space_.RemoveEntity(peer->Id());

  // Must NOT crash even though cache.entity is a dangling pointer.
  observer->GetWitness()->Update(4096);
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty())
      << "stale cache must be evicted, not dispatched";
}

// Observer movement: when the *observer* moves, its own trigger bounds
// must be resynced or peers it leaves behind get stranded in
// outer.inside_peers_.  Then when those peers die, vacate-shuffle's
// outer.OnLeave fires on a stale set and DispatchMembership bails
// (was==now both stale-true), HandleAoILeave never fires, cache stays
// dangling.
TEST_F(WitnessEnterPendingUafTest, ObserverMovesAwayThenPeerDies) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  auto* peer = MakeEntity(2, {3, 0, 3});
  observer->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {});

  // Peer enters at Update time (Activate's initial scan + first pump).
  observer->GetWitness()->Update(4096);
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  // Now teleport the observer FAR from peer.  With the fix in
  // CellEntity::SetPosition, OnOwnerMoved should resync trigger
  // bounds and fire HandleAoILeave for `peer`.
  observer->SetPosition({500, 0, 500});
  observer->GetWitness()->Update(4096);  // drains any pending leave
  EXPECT_TRUE(observer->GetWitness()->AoIMap().empty())
      << "peer should have been compacted out after observer left it behind";

  // Now destroy the peer.  If trigger bounds were left stale,
  // ~CellEntity's leave-fan-out audit would log an error.  ASan would
  // catch the UAF on a subsequent Update.  Both must be silent.
  space_.RemoveEntity(peer->Id());
  observer->GetWitness()->Update(4096);
}

// World_stress-style fuzz: many entities, all observers + peers, all
// random-walking concurrently, with periodic destructions.  Mirrors the
// load shape that exposed the original profile-build bug — every
// SetPosition fires OnOwnerMoved, every step risks a hysteresis
// re-cross, and inner-shuffles-before-outer can break the
// peer ∈ inner ⊂ outer invariant.  Under ASan, any leave-missed
// dangling pointer fires on the next Update; the audit in
// ~CellEntity reports any cache that survives destruction.
TEST_F(WitnessEnterPendingUafTest, FuzzRandomWalkAndDestroyMatchesWorldStress) {
  constexpr int kNumEntities = 64;
  constexpr int kSteps = 600;
  constexpr float kAoIRadius = 150.f;  // matches production default
  constexpr float kSpawnSpread = 500.f;
  constexpr float kWalkStep = 5.f;

  std::mt19937 rng(0xCAFEBABE);  // deterministic seed — bug must repro
  std::uniform_real_distribution<float> spawn(-kSpawnSpread, kSpawnSpread);
  std::uniform_real_distribution<float> step(-kWalkStep, kWalkStep);
  std::uniform_int_distribution<int> coin(0, 99);

  std::vector<EntityID> live_ids;
  live_ids.reserve(kNumEntities);
  for (int i = 0; i < kNumEntities; ++i) {
    const EntityID id = 1000 + i;
    auto* e = MakeEntity(id, {spawn(rng), 0.f, spawn(rng)});
    e->EnableWitness(kAoIRadius, [](EntityID, std::span<const std::byte>) {});
    live_ids.push_back(id);
  }

  EntityID next_id = 1000 + kNumEntities;
  for (int t = 0; t < kSteps; ++t) {
    // Random-walk every live entity through the production setter so
    // CellEntity::SetPosition → Witness::OnOwnerMoved gets exercised.
    for (auto id : live_ids) {
      auto* e = space_.FindEntity(id);
      ASSERT_NE(e, nullptr);
      const auto& p = e->Position();
      e->SetPosition({p.x + step(rng), 0.f, p.z + step(rng)});
    }
    // Drain witness updates so kEnterPending → cache.flags=0 transitions
    // run, leave envelopes flush, and the priority pump touches caches.
    for (auto id : live_ids) {
      if (auto* e = space_.FindEntity(id)) {
        if (auto* w = e->GetWitness()) w->Update(64 * 1024);
      }
    }
    // Liveness invariant: every non-null cache.entity must point to a
    // currently-live entity in space.entities_.  A dangling pointer is
    // exactly the condition my fixes target.
    for (auto id : live_ids) {
      auto* obs = space_.FindEntity(id);
      if (!obs) continue;
      auto* w = obs->GetWitness();
      if (!w) continue;
      for (const auto& [peer_id, cache] : w->AoIMap()) {
        if (cache.entity == nullptr) continue;
        ASSERT_NE(space_.FindEntity(peer_id), nullptr)
            << "obs=" << obs->Id() << " has dangling cache for peer=" << peer_id << " at tick "
            << t;
        EXPECT_EQ(space_.FindEntity(peer_id), cache.entity)
            << "obs=" << obs->Id() << " cache.entity stale for peer=" << peer_id;
      }
    }
    // Every ~50 ticks, churn the population: destroy a random entity and
    // spawn a new one.  Forces ~CellEntity's leave-fanout audit to run
    // while observers/peers are mid-flight.
    if (t > 0 && t % 50 == 0 && !live_ids.empty()) {
      const std::size_t idx = static_cast<std::size_t>(coin(rng)) % live_ids.size();
      space_.RemoveEntity(live_ids[idx]);
      live_ids[idx] = live_ids.back();
      live_ids.pop_back();

      const EntityID nid = next_id++;
      auto* e = MakeEntity(nid, {spawn(rng), 0.f, spawn(rng)});
      e->EnableWitness(kAoIRadius, [](EntityID, std::span<const std::byte>) {});
      live_ids.push_back(nid);
    }
  }
  // Tear-down: ~Space ⇒ each ~CellEntity's audit runs (skipped via
  // IsTearingDown but the live-entity invariants must already be clean).
}

// Recreate-with-same-id: peer enters, dies, a fresh peer is spawned at
// the same EntityID before Update runs.  The new peer ends up in the
// SAME aoi_map slot via try_emplace.  Verifies that the cache is
// re-bound to the live entity and SendEntityEnter operates on it.
TEST_F(WitnessEnterPendingUafTest, PeerRecreatedAtSameIdWhileEnterPending) {
  auto* observer = MakeEntity(1, {0, 0, 0});
  /*peer_v1 =*/MakeEntity(2, {3, 0, 3});

  observer->EnableWitness(10.f, [](EntityID, std::span<const std::byte>) {});
  ASSERT_EQ(observer->GetWitness()->AoIMap().size(), 1u);

  space_.RemoveEntity(2);
  // Re-create at the same id — common pattern in MMOs (respawn,
  // ghost→real transitions, offload).  HandleAoIEnter fires fresh.
  /*peer_v2 =*/MakeEntity(2, {2, 0, 2});

  observer->GetWitness()->Update(4096);
  observer->GetWitness()->Update(4096);
}

}  // namespace
}  // namespace atlas
