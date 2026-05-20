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
      987, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  Address addr(0x7F000001u, 1234);
  e->SetBaseAddr(addr);
  EXPECT_EQ(e->BaseAddr().Ip(), 0x7F000001u);
  EXPECT_EQ(e->BaseAddr().Port(), 1234u);
  EXPECT_EQ(e->Id(), 987u);
}

// ============================================================================
// PublishReplicationFrame
// ============================================================================

TEST(CellEntity, FirstEventFrameSeedsReplicationState) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  CellEntity::ReplicationFrame frame;
  frame.owner_delta = MakeBlob({0xAA, 0xBB});
  frame.other_delta = MakeBlob({0xCC});
  auto owner_snap = MakeBlob({0x11, 0x22, 0x33});
  auto other_snap = MakeBlob({0x44});

  e->PublishReplicationFrame(frame, /*has_event=*/true, /*has_volatile=*/false, owner_snap,
                             other_snap);

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
  // so the deque stays bounded. Engine auto-increments seq.
  const auto window = CellEntity::kReplicationHistoryWindow;
  for (std::size_t i = 0; i < window + 4; ++i) {
    CellEntity::ReplicationFrame frame;
    e->PublishReplicationFrame(frame, /*has_event=*/true, /*has_volatile=*/false, {}, {});
  }
  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, static_cast<uint64_t>(window + 4));
  EXPECT_EQ(state->history.size(), window);
  // Oldest surviving frame's event_seq should be (total - window + 1).
  EXPECT_EQ(state->history.front().event_seq, 5u);
  EXPECT_EQ(state->history.back().event_seq, static_cast<uint64_t>(window + 4));
}

TEST(CellEntity, VolatileFrameUpdatesPosition) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  CellEntity::ReplicationFrame frame;
  frame.position = {10.f, 0.f, 20.f};
  frame.direction = {0.f, 0.f, 1.f};
  frame.on_ground = true;

  e->PublishReplicationFrame(frame, /*has_event=*/false, /*has_volatile=*/true, {}, {});

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
  frame.position = {1, 0, 1};
  e->PublishReplicationFrame(frame, /*has_event=*/true, /*has_volatile=*/true, MakeBlob({0x01}),
                             {});

  const auto* state = e->GetReplicationState();
  EXPECT_EQ(state->latest_event_seq, 1u);
  EXPECT_EQ(state->latest_volatile_seq, 1u);
}

TEST(CellEntity, BothFlagsFalseIsNoop) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  CellEntity::ReplicationFrame frame;
  e->PublishReplicationFrame(frame, /*has_event=*/false, /*has_volatile=*/false, {}, {});
  // Early return — state never even allocated.
  EXPECT_EQ(e->GetReplicationState(), nullptr);
}

TEST(CellEntity, SeedReplicationStateAdoptsSeqs) {
  Space space(1);
  auto* e = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  auto owner_snap = MakeBlob({0xAA});
  auto other_snap = MakeBlob({0xBB});
  e->SeedReplicationState(42, 99, owner_snap, other_snap);

  const auto* state = e->GetReplicationState();
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->latest_event_seq, 42u);
  EXPECT_EQ(state->latest_volatile_seq, 99u);

  // Subsequent publish continues monotonically from seeded values.
  CellEntity::ReplicationFrame f;
  e->PublishReplicationFrame(f, /*has_event=*/true, /*has_volatile=*/true, {}, {});
  EXPECT_EQ(state->latest_event_seq, 43u);
  EXPECT_EQ(state->latest_volatile_seq, 100u);
}

}  // namespace
}  // namespace atlas
