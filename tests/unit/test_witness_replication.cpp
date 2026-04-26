// Witness replication-delta tests.
//
// Exercises the per-peer catch-up pump that Witness::Update invokes
// once per AoI entry per tick. Focuses on:
//   - volatile position updates (latest-wins, jumps to newest seq)
//   - property event replay (ordered, consumes history frames one by one)
//   - snapshot fallback when the observer falls beyond the history window
//   - owner-vs-other audience selection driven by BaseEntityId equality

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "witness.h"

namespace atlas {
namespace {

struct Captured {
  EntityID observer_base_id{0};
  std::vector<std::byte> payload;
  bool reliable{true};
};

auto KindOf(const Captured& c) -> CellAoIEnvelopeKind {
  return static_cast<CellAoIEnvelopeKind>(c.payload.at(0));
}

auto PayloadBody(const Captured& c) -> std::span<const std::byte> {
  // Skip 1-byte kind + 4-byte public_entity_id.
  return std::span<const std::byte>(c.payload.data() + 5, c.payload.size() - 5);
}

// kEntityPropertyUpdate envelopes prepend a uint64 LE event_seq to their
// delta/snapshot bytes; the two helpers below extract each half.
auto PropertyUpdateSeq(const Captured& c) -> uint64_t {
  auto body = PayloadBody(c);
  uint64_t seq = 0;
  for (int i = 0; i < 8; ++i) {
    seq |= static_cast<uint64_t>(static_cast<unsigned char>(body[i])) << (i * 8);
  }
  return seq;
}

auto PropertyUpdateDelta(const Captured& c) -> std::span<const std::byte> {
  auto body = PayloadBody(c);
  return body.subspan(8);
}

auto MakeBlob(std::initializer_list<uint8_t> bytes) -> std::vector<std::byte> {
  std::vector<std::byte> v;
  v.reserve(bytes.size());
  for (auto b : bytes) v.push_back(static_cast<std::byte>(b));
  return v;
}

// Build a minimal replication frame carrying distinct owner/other deltas
// so audience selection is trivially checkable on receipt.
auto MakeFrame(uint64_t event_seq, std::vector<std::byte> owner_delta = {},
               std::vector<std::byte> other_delta = {}) {
  CellEntity::ReplicationFrame f;
  f.event_seq = event_seq;
  f.owner_delta = std::move(owner_delta);
  f.other_delta = std::move(other_delta);
  return f;
}

class WitnessReplicationTest : public ::testing::Test {
 protected:
  std::vector<Captured> sent_;

  auto MakeReliable() {
    return [this](EntityID obs, std::span<const std::byte> env) {
      sent_.push_back({obs, std::vector<std::byte>(env.begin(), env.end()), /*reliable=*/true});
    };
  }
  auto MakeUnreliable() {
    return [this](EntityID obs, std::span<const std::byte> env) {
      sent_.push_back({obs, std::vector<std::byte>(env.begin(), env.end()), /*reliable=*/false});
    };
  }

  static auto MakeEntity(Space& space, EntityID id, EntityID base_id, math::Vector3 pos)
      -> CellEntity* {
    auto* e = space.AddEntity(
        std::make_unique<CellEntity>(id, /*type_id=*/1, space, pos, math::Vector3{1, 0, 0}));
    e->SetBase(Address(0, 0), base_id);
    return e;
  }
};

// ----------------------------------------------------------------------------
// Volatile position updates
// ----------------------------------------------------------------------------

TEST_F(WitnessReplicationTest, VolatileJumpsToLatestSeq) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, /*base=*/1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, /*base=*/1002, {3, 0, 3});

  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  // Publish a volatile update on the peer.
  CellEntity::ReplicationFrame v1;
  v1.volatile_seq = 1;
  v1.position = {5, 0, 5};
  v1.direction = {0, 0, 1};
  v1.on_ground = true;
  peer->PublishReplicationFrame(v1, {}, {});

  // Fetch the cache entry that AoI tracking inserted for the peer and
  // drive the pump directly.
  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  // Enter pass consumed the initial entry; clear flags so the pump is
  // in its "updatable" path.
  cache.flags = 0;
  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kEntityPositionUpdate);
  EXPECT_FALSE(sent_[0].reliable) << "Volatile path must route to the unreliable delivery";
  EXPECT_EQ(cache.last_volatile_seq, 1u);
}

TEST_F(WitnessReplicationTest, VolatileNoOpWhenUpToDate) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, 1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  CellEntity::ReplicationFrame v1;
  v1.volatile_seq = 5;
  v1.position = {1, 0, 1};
  peer->PublishReplicationFrame(v1, {}, {});
  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_volatile_seq = 5;  // already current

  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);
  // No volatile envelope emitted — cache is already caught up.
  for (const auto& c : sent_) {
    EXPECT_NE(KindOf(c), CellAoIEnvelopeKind::kEntityPositionUpdate);
  }
}

// ----------------------------------------------------------------------------
// Property delta replay
// ----------------------------------------------------------------------------

TEST_F(WitnessReplicationTest, PropertyDeltasReplayInOrder) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, 1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  // Publish 3 frames (1..3). Each frame's other_delta carries the
  // per-frame marker byte; the observer is not the peer's owning
  // client, so the witness replays frame.other_delta.
  peer->PublishReplicationFrame(MakeFrame(1, /*owner=*/MakeBlob({0xA1}),
                                          /*other=*/MakeBlob({0xB1})),
                                MakeBlob({0xA1}), MakeBlob({0xB1}));
  peer->PublishReplicationFrame(MakeFrame(2, MakeBlob({0xA2}), MakeBlob({0xB2})), MakeBlob({0xA2}),
                                MakeBlob({0xB2}));
  peer->PublishReplicationFrame(MakeFrame(3, MakeBlob({0xA3}), MakeBlob({0xB3})), MakeBlob({0xA3}),
                                MakeBlob({0xB3}));

  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;
  cache.last_volatile_seq = 0;

  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  // Should emit exactly 3 EntityPropertyUpdate envelopes carrying
  // other_delta bytes in order (other_delta used because observer is
  // NOT the peer's owning client).
  std::vector<Captured> updates;
  for (auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) updates.push_back(c);
  }
  ASSERT_EQ(updates.size(), 3u);
  EXPECT_EQ(PropertyUpdateSeq(updates[0]), 1u);
  EXPECT_EQ(PropertyUpdateSeq(updates[1]), 2u);
  EXPECT_EQ(PropertyUpdateSeq(updates[2]), 3u);
  EXPECT_EQ(PropertyUpdateDelta(updates[0])[0], std::byte{0xB1});
  EXPECT_EQ(PropertyUpdateDelta(updates[1])[0], std::byte{0xB2});
  EXPECT_EQ(PropertyUpdateDelta(updates[2])[0], std::byte{0xB3});
  EXPECT_EQ(cache.last_event_seq, 3u);
  for (const auto& u : updates) EXPECT_TRUE(u.reliable);
}

// A frame whose per-audience delta is the flag-prefix-only zero byte
// means "no audience-visible field touched this frame" — the peer
// dirtied only owner-scope props while this observer is the other
// audience. Shipping the 1-byte envelope would burn wire for nothing.
// The witness must skip the send but still advance cache.last_event_seq
// so the next non-empty frame doesn't look like a gap.
TEST_F(WitnessReplicationTest, AllZeroDeltaIsSkippedButSeqAdvances) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, 1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  // Frame with a non-empty owner_delta but an all-zero other_delta
  // (the flag prefix said "no other-audience fields dirty").
  peer->PublishReplicationFrame(MakeFrame(1, MakeBlob({0xA1}), MakeBlob({0x00})), MakeBlob({0xA1}),
                                MakeBlob({0x00}));

  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;

  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  // No EntityPropertyUpdate — the all-zero delta was suppressed.
  for (const auto& c : sent_) {
    EXPECT_NE(KindOf(c), CellAoIEnvelopeKind::kEntityPropertyUpdate);
  }
  // Seq advanced so a later non-empty frame isn't mistaken for a gap.
  EXPECT_EQ(cache.last_event_seq, 1u);
}

TEST_F(WitnessReplicationTest, PropertyDeltaOwnerAudienceWhenObserverIsOwner) {
  Space space(1);
  // Observer IS the peer's owner: both entities share a base_entity_id
  // (contrived, but realistic for the owner-watches-own-cell case).
  auto* observer = MakeEntity(space, 1, /*base=*/1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, /*base=*/1001, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  peer->PublishReplicationFrame(MakeFrame(1, MakeBlob({0xCC}), MakeBlob({0xDD})), {}, {});
  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;

  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);
  std::vector<Captured> updates;
  for (auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) updates.push_back(c);
  }
  ASSERT_EQ(updates.size(), 1u);
  // Owner-audience byte (0xCC) not other-audience (0xDD).
  EXPECT_EQ(PropertyUpdateDelta(updates[0])[0], std::byte{0xCC});
  EXPECT_EQ(PropertyUpdateSeq(updates[0]), 1u);
}

// ----------------------------------------------------------------------------
// Snapshot fallback — observer beyond the history window
// ----------------------------------------------------------------------------

TEST_F(WitnessReplicationTest, SnapshotFallbackWhenBeyondHistoryWindow) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, 1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  // Publish more frames than the history window so earlier ones are
  // evicted. Each frame carries a distinct other_delta so we can assert
  // the test didn't accidentally fall through to a delta pathway.
  const auto window = CellEntity::kReplicationHistoryWindow;
  for (uint64_t i = 1; i <= window + 4; ++i) {
    peer->PublishReplicationFrame(MakeFrame(i, {}, MakeBlob({static_cast<uint8_t>(i)})),
                                  MakeBlob({0x11}), MakeBlob({0x22, 0x33}));  // snapshots
  }

  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;  // pretend we never saw anything — beyond window

  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);
  std::vector<Captured> updates;
  for (auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) updates.push_back(c);
  }
  ASSERT_EQ(updates.size(), 1u) << "Fallback ships the snapshot in ONE envelope, not history";

  // Payload should be the peer's other_snapshot (two bytes {0x22, 0x33}),
  // preceded by the latest_event_seq the snapshot reflects.
  auto delta = PropertyUpdateDelta(updates[0]);
  ASSERT_EQ(delta.size(), 2u);
  EXPECT_EQ(delta[0], std::byte{0x22});
  EXPECT_EQ(delta[1], std::byte{0x33});
  EXPECT_EQ(PropertyUpdateSeq(updates[0]), window + 4);

  // After fallback, last_event_seq jumps to the peer's current latest.
  EXPECT_EQ(cache.last_event_seq, window + 4);
}

TEST_F(WitnessReplicationTest, SnapshotFallbackFollowedByIncrementalCatchup) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, 1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  const auto window = CellEntity::kReplicationHistoryWindow;
  // Overflow history → snapshot fallback pulls us up to event_seq=window+4.
  for (uint64_t i = 1; i <= window + 4; ++i) {
    peer->PublishReplicationFrame(MakeFrame(i, {}, MakeBlob({0xAA})), MakeBlob({0xBB}),
                                  MakeBlob({0xCC}));
  }
  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 0;
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);
  ASSERT_EQ(cache.last_event_seq, window + 4);

  // New frame; observer is inside window now, so catch-up should be
  // incremental (history replay of frame.other_delta, not the other_snapshot).
  peer->PublishReplicationFrame(MakeFrame(window + 5, MakeBlob({0xBB}), MakeBlob({0xEF})),
                                MakeBlob({0xBB}), MakeBlob({0xCC}));
  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);

  std::vector<Captured> updates;
  for (auto& c : sent_) {
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) updates.push_back(c);
  }
  ASSERT_EQ(updates.size(), 1u);
  auto delta = PropertyUpdateDelta(updates[0]);
  EXPECT_EQ(delta.size(), 1u);
  EXPECT_EQ(delta[0], std::byte{0xEF}) << "History replay should ship frame.other_delta";
  EXPECT_EQ(PropertyUpdateSeq(updates[0]), window + 5);
  EXPECT_EQ(cache.last_event_seq, window + 5);
}

TEST_F(WitnessReplicationTest, UpdateEmitsNothingWhenFullyCaughtUp) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer = MakeEntity(space, 2, 1002, {3, 0, 3});
  observer->EnableWitness(10.f, MakeReliable(), MakeUnreliable());

  peer->PublishReplicationFrame(MakeFrame(1, {}, MakeBlob({0xAA})), MakeBlob({0x01}),
                                MakeBlob({0x02}));
  auto& cache = observer->GetWitness()->AoIMapMutable().at(peer->Id());
  cache.flags = 0;
  cache.last_event_seq = 1;
  cache.last_volatile_seq = 0;  // no volatile ever published → also caught up

  sent_.clear();
  observer->GetWitness()->TestOnlySendEntityUpdate(cache);
  EXPECT_TRUE(sent_.empty());
}

// ----------------------------------------------------------------------------
// Distance LOD scheduling
// ----------------------------------------------------------------------------

// Helper: count kEntityPositionUpdate envelopes collected since last clear.
auto CountPositionUpdates(const std::vector<Captured>& sent) -> int {
  int n = 0;
  for (const auto& c : sent)
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPositionUpdate) ++n;
  return n;
}

// Run N full Update() ticks, publishing a fresh volatile frame each tick.
// Returns a per-tick vector of position-update counts received.
auto RunTicks(Witness& witness, CellEntity& peer, int ticks, uint32_t budget = 65536)
    -> std::vector<int> {
  std::vector<int> counts;
  uint64_t seq = 0;
  for (int i = 0; i < ticks; ++i) {
    CellEntity::ReplicationFrame f;
    f.volatile_seq = ++seq;
    peer.PublishReplicationFrame(f, {}, {});
    // Clear the Enter envelope emitted on tick 0.
    std::vector<Captured> tick_sent;
    // Can't intercept per-tick easily via the stored callback; use AoIMap
    // to assert lod_next_update_tick instead — see below tests.
    witness.Update(budget);
  }
  return counts;
}

// Close peers (< 50 m) must be updated every tick.
TEST_F(WitnessReplicationTest, LodCloseUpdatesEveryTick) {
  Space space(1);
  // Observer at origin; peer at 10 m — well within the Close band (< 25 m).
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer     = MakeEntity(space, 2, 1002, {10, 0, 0});
  observer->EnableWitness(500.f, MakeReliable(), MakeUnreliable());

  const math::Vector3 kPeerPos{10, 0, 0};
  const int kTicks = 6;
  int pos_updates = 0;
  for (int t = 0; t < kTicks; ++t) {
    CellEntity::ReplicationFrame f;
    f.volatile_seq = static_cast<uint64_t>(t + 1);
    f.position = kPeerPos;  // keep peer at its initial position
    peer->PublishReplicationFrame(f, {}, {});
    sent_.clear();
    observer->GetWitness()->Update(65536);
    pos_updates += CountPositionUpdates(sent_);
  }
  // Tick 1 (enter): Enter captures volatile_seq=1 → Pump sees 1>1, no pos update.
  // Ticks 2-6: interval=1, every tick → 5 updates.
  EXPECT_EQ(pos_updates, kTicks - 1);
}

// Medium peers (25–100 m) must be updated every 3rd tick.
TEST_F(WitnessReplicationTest, LodMediumUpdatesEvery3Ticks) {
  Space space(1);
  // Peer at 50 m — inside the Medium band (25–100 m).
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer     = MakeEntity(space, 2, 1002, {50, 0, 0});
  observer->EnableWitness(500.f, MakeReliable(), MakeUnreliable());

  const math::Vector3 kPeerPos{50, 0, 0};
  const int kTicks = 9;
  int pos_updates = 0;
  for (int t = 0; t < kTicks; ++t) {
    CellEntity::ReplicationFrame f;
    f.volatile_seq = static_cast<uint64_t>(t + 1);
    f.position = kPeerPos;
    peer->PublishReplicationFrame(f, {}, {});
    sent_.clear();
    observer->GetWitness()->Update(65536);
    pos_updates += CountPositionUpdates(sent_);
  }
  // Tick 1 (enter): no pos update (enter captures seq). lod_next=4.
  // Tick 4: update (volatile caught up). lod_next=7.
  // Tick 7: update. lod_next=10.
  // Total: 2 updates in 9 ticks.
  EXPECT_EQ(pos_updates, 2);
}

// Far peers (≥ 100 m) must be updated every 6th tick.
TEST_F(WitnessReplicationTest, LodFarUpdatesEvery6Ticks) {
  Space space(1);
  // Peer at 150 m — inside the Far band (≥ 100 m).
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer     = MakeEntity(space, 2, 1002, {150, 0, 0});
  observer->EnableWitness(500.f, MakeReliable(), MakeUnreliable());

  const math::Vector3 kPeerPos{150, 0, 0};
  const int kTicks = 12;
  int pos_updates = 0;
  for (int t = 0; t < kTicks; ++t) {
    CellEntity::ReplicationFrame f;
    f.volatile_seq = static_cast<uint64_t>(t + 1);
    f.position = kPeerPos;
    peer->PublishReplicationFrame(f, {}, {});
    sent_.clear();
    observer->GetWitness()->Update(65536);
    pos_updates += CountPositionUpdates(sent_);
  }
  // Tick 1 (enter): no pos update. lod_next=7.
  // Tick 7: update. lod_next=13.
  // Ticks 8-12: all < 13 → skip.
  // Total: 1 update in 12 ticks.
  EXPECT_EQ(pos_updates, 1);
}

// Event-stream deltas for a far peer must be replayed when the peer
// is finally processed, without snapshot fallback. The history window
// (8 frames) covers 6-tick LOD gaps at 10 Hz comfortably.
TEST_F(WitnessReplicationTest, LodFarEventDeliveredOnNextWindow) {
  Space space(1);
  auto* observer = MakeEntity(space, 1, 1001, {0, 0, 0});
  auto* peer     = MakeEntity(space, 2, 1002, {150, 0, 0});
  observer->EnableWitness(500.f, MakeReliable(), MakeUnreliable());

  // Tick 1 — initial enter handled; peer gets its first LOD update.
  sent_.clear();
  observer->GetWitness()->Update(65536);

  // Publish a property event on tick 2; peer won't be polled again until
  // tick 7 (far interval = 6).
  peer->PublishReplicationFrame(
      MakeFrame(1, {}, MakeBlob({0x01, 0xAB})),
      MakeBlob({0xFF}), MakeBlob({0xFE}));

  // Ticks 2-6: property delta accumulates in history, peer is LOD-skipped.
  for (int t = 0; t < 5; ++t) {
    sent_.clear();
    observer->GetWitness()->Update(65536);
    // No property update should arrive during the skip window.
    for (const auto& c : sent_)
      EXPECT_NE(KindOf(c), CellAoIEnvelopeKind::kEntityPropertyUpdate)
          << "LOD-skipped peer must not emit property updates on tick " << (t + 2);
  }

  // Tick 7: peer re-enters queue, history replay delivers the delta.
  sent_.clear();
  observer->GetWitness()->Update(65536);
  int prop_updates = 0;
  for (const auto& c : sent_)
    if (KindOf(c) == CellAoIEnvelopeKind::kEntityPropertyUpdate) ++prop_updates;
  EXPECT_EQ(prop_updates, 1) << "Far peer must deliver queued delta on re-entry tick";
}

}  // namespace
}  // namespace atlas
