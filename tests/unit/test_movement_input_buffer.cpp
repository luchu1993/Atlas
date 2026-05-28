#include <vector>

#include <gtest/gtest.h>

#include "movement_input_buffer.h"

using namespace atlas;

namespace {

auto Frame(uint32_t seq) -> movement::InputFrame {
  movement::InputFrame frame;
  frame.seq = seq;
  frame.input_tick = seq + 100;
  frame.client_dt_ms = movement::kMinInputDtMs;
  return frame;
}

}  // namespace

TEST(MovementInputBuffer, EnqueuesNewFramesAndDropsStale) {
  MovementInputBuffer buffer;
  const std::vector<movement::InputFrame> first{Frame(10), Frame(11)};

  auto result = buffer.Enqueue(42, first);
  EXPECT_EQ(result.accepted, 2u);
  EXPECT_EQ(result.dropped_stale, 0u);
  EXPECT_EQ(buffer.QueueDepth(42), 2u);
  EXPECT_EQ(buffer.LastAcceptedSeq(42), 11u);

  const std::vector<movement::InputFrame> second{Frame(11), Frame(9), Frame(12)};
  result = buffer.Enqueue(42, second);
  EXPECT_EQ(result.accepted, 1u);
  EXPECT_EQ(result.dropped_stale, 2u);
  EXPECT_EQ(buffer.QueueDepth(42), 3u);
  EXPECT_EQ(buffer.LastAcceptedSeq(42), 12u);
}

TEST(MovementInputBuffer, CapacityDropsOldestFrames) {
  MovementInputBuffer buffer(2);
  const std::vector<movement::InputFrame> frames{Frame(1), Frame(2), Frame(3)};

  auto result = buffer.Enqueue(7, frames);
  EXPECT_EQ(result.accepted, 3u);
  EXPECT_EQ(result.dropped_overflow, 1u);
  EXPECT_EQ(buffer.QueueDepth(7), 2u);

  auto drained = buffer.Drain(7, 8);
  ASSERT_EQ(drained.size(), 2u);
  EXPECT_EQ(drained[0].seq, 2u);
  EXPECT_EQ(drained[1].seq, 3u);
  EXPECT_EQ(buffer.TotalQueueDepth(), 0u);
}

TEST(MovementInputBuffer, DropsImplausibleSequenceGap) {
  MovementInputBuffer buffer(32, 4);
  const std::vector<movement::InputFrame> first{Frame(10)};
  auto result = buffer.Enqueue(7, first);
  EXPECT_EQ(result.accepted, 1u);

  const std::vector<movement::InputFrame> jump{Frame(20), Frame(11)};
  result = buffer.Enqueue(7, jump);
  EXPECT_EQ(result.accepted, 1u);
  EXPECT_EQ(result.dropped_gap, 1u);
  EXPECT_EQ(buffer.LastAcceptedSeq(7), 11u);
}

TEST(MovementInputBuffer, DropsInvalidFramesWithoutAdvancingSequence) {
  MovementInputBuffer buffer;
  auto invalid = Frame(1);
  invalid.client_dt_ms = 0;
  const std::vector<movement::InputFrame> frames{invalid, Frame(1)};

  auto result = buffer.Enqueue(7, frames);

  EXPECT_EQ(result.accepted, 1u);
  EXPECT_EQ(result.dropped_invalid, 1u);
  EXPECT_EQ(result.dropped_stale, 0u);
  EXPECT_EQ(buffer.LastAcceptedSeq(7), 1u);
}

TEST(MovementInputBuffer, ClassifiesInvalidEntitySeparatelyFromOverflow) {
  MovementInputBuffer buffer;
  const std::vector<movement::InputFrame> frames{Frame(1), Frame(2)};

  auto result = buffer.Enqueue(kInvalidEntityID, frames);

  EXPECT_EQ(result.accepted, 0u);
  EXPECT_EQ(result.dropped_invalid, 2u);
  EXPECT_EQ(result.dropped_overflow, 0u);
  EXPECT_EQ(buffer.TotalQueueDepth(), 0u);
}

TEST(MovementInputBuffer, ZeroCapacityDropsAsOverflow) {
  MovementInputBuffer buffer(0);
  const std::vector<movement::InputFrame> frames{Frame(1), Frame(2)};

  auto result = buffer.Enqueue(7, frames);

  EXPECT_EQ(result.accepted, 0u);
  EXPECT_EQ(result.dropped_invalid, 0u);
  EXPECT_EQ(result.dropped_overflow, 2u);
  EXPECT_EQ(buffer.TotalQueueDepth(), 0u);
}

TEST(MovementInputBuffer, DrainRespectsFrameBudget) {
  MovementInputBuffer buffer;
  const std::vector<movement::InputFrame> frames{Frame(1), Frame(2), Frame(3)};
  (void)buffer.Enqueue(7, frames);

  auto drained = buffer.Drain(7, 2);
  ASSERT_EQ(drained.size(), 2u);
  EXPECT_EQ(drained[0].seq, 1u);
  EXPECT_EQ(drained[1].seq, 2u);
  EXPECT_EQ(buffer.QueueDepth(7), 1u);
  EXPECT_EQ(buffer.TotalQueueDepth(), 1u);

  drained = buffer.Drain(7, 2);
  ASSERT_EQ(drained.size(), 1u);
  EXPECT_EQ(drained[0].seq, 3u);
  EXPECT_EQ(buffer.QueueDepth(7), 0u);

  const std::vector<movement::InputFrame> stale{Frame(3)};
  auto result = buffer.Enqueue(7, stale);
  EXPECT_EQ(result.accepted, 0u);
  EXPECT_EQ(result.dropped_stale, 1u);
}

TEST(MovementInputBuffer, AppendsEntitiesWithPendingInput) {
  MovementInputBuffer buffer;
  const std::vector<movement::InputFrame> frames{Frame(1)};
  (void)buffer.Enqueue(7, frames);
  (void)buffer.Enqueue(9, frames);
  (void)buffer.Drain(7, 1);

  std::vector<EntityID> ids;
  buffer.AppendEntityIdsWithPendingInput(ids);

  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], 9u);
}
