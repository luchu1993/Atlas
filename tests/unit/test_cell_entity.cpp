// CellEntity tests.
//
// Focus on the non-Space-mechanics surface: identity, position via the
// IEntityMotion interface, base mailbox plumbing, the replication frame
// pipeline, and destroy-order ordering.

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"

namespace atlas {
namespace {

auto MakeBlob(std::initializer_list<uint8_t> bytes) -> std::vector<std::byte> {
  std::vector<std::byte> v;
  v.reserve(bytes.size());
  for (auto b : bytes) v.push_back(static_cast<std::byte>(b));
  return v;
}

TEST(CellEntity, IdentityAndInitialState) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      42, /*type_id=*/uint16_t{7}, space, math::Vector3{1, 2, 3}, math::Vector3{0, 0, 1}));
  EXPECT_EQ(e->Id(), 42u);
  EXPECT_EQ(e->TypeId(), 7u);
  EXPECT_EQ(e->Position().x, 1.f);
  EXPECT_EQ(e->Direction().z, 1.f);
  EXPECT_FALSE(e->OnGround());
  EXPECT_FALSE(e->IsDestroyed());
  EXPECT_EQ(e->ScriptHandle(), 0u);
  EXPECT_EQ(e->BaseEntityId(), kInvalidEntityID);
  EXPECT_EQ(e->GetReplicationState(), nullptr);
}

TEST(CellEntity, SetPositionUpdatesRangeNode) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  e->SetPosition(math::Vector3{5, 0, 7});
  EXPECT_FLOAT_EQ(e->Position().x, 5.f);
  EXPECT_FLOAT_EQ(e->Position().z, 7.f);
  EXPECT_FLOAT_EQ(e->RangeNode().X(), 5.f);
  EXPECT_FLOAT_EQ(e->RangeNode().Z(), 7.f);
}

TEST(CellEntity, SetDirectionDoesNotShuffleRangeList) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{5, 0, 5}, math::Vector3{1, 0, 0}));
  const float x_before = e->RangeNode().X();
  e->SetDirection(math::Vector3{0, 0, 1});
  EXPECT_FLOAT_EQ(e->Direction().z, 1.f);
  // Range node untouched.
  EXPECT_FLOAT_EQ(e->RangeNode().X(), x_before);
}

TEST(CellEntity, BaseMailboxPlumbing) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  Address addr(0x7F000001u, 1234);
  e->SetBase(addr, /*base_id=*/987);
  EXPECT_EQ(e->BaseAddr().Ip(), 0x7F000001u);
  EXPECT_EQ(e->BaseAddr().Port(), 1234u);
  EXPECT_EQ(e->BaseEntityId(), 987u);
}

// ============================================================================
// PublishReplicationFrame
// ============================================================================

TEST(CellEntity, FirstEventFrameSeedsReplicationState) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  CellEntity::ReplicationFrame frame;
  frame.event_seq = 1;
  frame.owner_delta = MakeBlob({0xAA, 0xBB});
  frame.other_delta = MakeBlob({0xCC});
  auto owner_snap = MakeBlob({0x11, 0x22, 0x33});
  auto other_snap = MakeBlob({0x44});

  e->PublishReplicationFrame(frame, owner_snap, other_snap);

  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, 1u);
  EXPECT_EQ(state->latest_volatile_seq, 0u);
  EXPECT_EQ(state->owner_snapshot.size(), 3u);
  EXPECT_EQ(state->other_snapshot.size(), 1u);
  ASSERT_EQ(state->history.size(), 1u);
  EXPECT_EQ(state->history.front().event_seq, 1u);
}

TEST(CellEntity, HistoryWindowBounded) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  // Publish more frames than the window size; old ones must be evicted
  // so the deque stays bounded.
  const auto window = CellEntity::kReplicationHistoryWindow;
  for (uint64_t i = 1; i <= window + 4; ++i) {
    CellEntity::ReplicationFrame frame;
    frame.event_seq = i;
    e->PublishReplicationFrame(frame, {}, {});
  }
  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, static_cast<uint64_t>(window + 4));
  EXPECT_EQ(state->history.size(), window);
  // Oldest surviving frame's event_seq should be (total - window + 1).
  EXPECT_EQ(state->history.front().event_seq, 5u);
  EXPECT_EQ(state->history.back().event_seq, static_cast<uint64_t>(window + 4));
}

TEST(CellEntity, StaleEventSeqIsIgnored) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  CellEntity::ReplicationFrame f1;
  f1.event_seq = 5;
  e->PublishReplicationFrame(f1, MakeBlob({0x01}), {});

  CellEntity::ReplicationFrame f_stale;
  f_stale.event_seq = 3;
  f_stale.owner_delta = MakeBlob({0x99});
  e->PublishReplicationFrame(f_stale, MakeBlob({0xFF}), {});

  const auto* state = e->GetReplicationState();
  EXPECT_EQ(state->latest_event_seq, 5u);
  // Snapshot untouched — stale frame didn't overwrite.
  ASSERT_EQ(state->owner_snapshot.size(), 1u);
  EXPECT_EQ(state->owner_snapshot[0], std::byte{0x01});
}

TEST(CellEntity, VolatileFrameUpdatesPosition) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  CellEntity::ReplicationFrame frame;
  frame.volatile_seq = 1;
  frame.position = {10.f, 0.f, 20.f};
  frame.direction = {0.f, 0.f, 1.f};
  frame.on_ground = true;

  e->PublishReplicationFrame(frame, {}, {});

  EXPECT_FLOAT_EQ(e->Position().x, 10.f);
  EXPECT_FLOAT_EQ(e->Position().z, 20.f);
  EXPECT_FLOAT_EQ(e->Direction().z, 1.f);
  EXPECT_TRUE(e->OnGround());
  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_volatile_seq, 1u);
}

TEST(CellEntity, CombinedFrameAdvancesBothSeqs) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  CellEntity::ReplicationFrame frame;
  frame.event_seq = 1;
  frame.volatile_seq = 1;
  frame.position = {1, 0, 1};
  e->PublishReplicationFrame(frame, MakeBlob({0x01}), {});

  const auto* state = e->GetReplicationState();
  EXPECT_EQ(state->latest_event_seq, 1u);
  EXPECT_EQ(state->latest_volatile_seq, 1u);
}

TEST(CellEntity, ZeroZeroFrameIsNoop) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  CellEntity::ReplicationFrame frame;  // both seqs default zero
  e->PublishReplicationFrame(frame, {}, {});
  // State is still initialised (emplace happens regardless) but no
  // event or volatile progress.
  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, 0u);
  EXPECT_EQ(state->latest_volatile_seq, 0u);
  EXPECT_TRUE(state->history.empty());
}

}  // namespace
}  // namespace atlas
