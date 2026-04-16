#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "network/seq_num.h"

using namespace atlas;

TEST(SeqNum, LessThanNormal) {
  EXPECT_TRUE(SeqLessThan(1, 2));
  EXPECT_TRUE(SeqLessThan(100, 200));
  EXPECT_FALSE(SeqLessThan(2, 1));
  EXPECT_FALSE(SeqLessThan(5, 5));
}

TEST(SeqNum, LessThanWrapping) {
  // Near uint32 boundary
  constexpr SeqNum near_max = std::numeric_limits<uint32_t>::max() - 5;
  constexpr SeqNum near_zero = 5;
  // near_max comes BEFORE near_zero in wrapping space
  EXPECT_TRUE(SeqLessThan(near_max, near_zero));
  EXPECT_FALSE(SeqLessThan(near_zero, near_max));
}

TEST(SeqNum, GreaterThanNormal) {
  EXPECT_TRUE(SeqGreaterThan(10, 5));
  EXPECT_FALSE(SeqGreaterThan(5, 10));
  EXPECT_FALSE(SeqGreaterThan(5, 5));
}

TEST(SeqNum, GreaterThanWrapping) {
  constexpr SeqNum near_max = std::numeric_limits<uint32_t>::max() - 2;
  constexpr SeqNum near_zero = 3;
  EXPECT_TRUE(SeqGreaterThan(near_zero, near_max));
}

TEST(SeqNum, DiffNormal) {
  EXPECT_EQ(SeqDiff(10, 5), 5);
  EXPECT_EQ(SeqDiff(5, 10), -5);
}

TEST(SeqNum, DiffWrapping) {
  constexpr SeqNum near_max = std::numeric_limits<uint32_t>::max();
  EXPECT_EQ(SeqDiff(1, near_max), 2);  // 1 is 2 ahead of MAX
  EXPECT_EQ(SeqDiff(near_max, 1), -2);
}

TEST(SeqNum, AckBitsSetAndTest) {
  uint32_t bits = 0;
  SeqNum ack_num = 10;

  // Set bit for seq 9 (ack_num - 1 = diff 1, bit 0)
  AckBitsSet(bits, ack_num, 9);
  EXPECT_TRUE(AckBitsTest(bits, ack_num, 9));

  // Set bit for seq 7 (diff 3, bit 2)
  AckBitsSet(bits, ack_num, 7);
  EXPECT_TRUE(AckBitsTest(bits, ack_num, 7));

  // ack_num itself is always considered ACK'd
  EXPECT_TRUE(AckBitsTest(bits, ack_num, 10));

  // Seq 8 not set
  EXPECT_FALSE(AckBitsTest(bits, ack_num, 8));
}

TEST(SeqNum, AckBitsOutOfRange) {
  uint32_t bits = 0xFFFFFFFF;
  SeqNum ack_num = 100;

  // Too old (diff > 32)
  EXPECT_FALSE(AckBitsTest(bits, ack_num, 50));

  // Future (seq > ack_num)
  EXPECT_FALSE(AckBitsTest(bits, ack_num, 101));
}

TEST(SeqNum, AckBitsFullWindow) {
  uint32_t bits = 0;
  SeqNum ack_num = 50;

  // Set all 32 bits
  for (SeqNum s = 18; s < 50; ++s) {
    AckBitsSet(bits, ack_num, s);
  }

  // All should test true
  for (SeqNum s = 18; s <= 50; ++s) {
    EXPECT_TRUE(AckBitsTest(bits, ack_num, s)) << "Failed for seq=" << s;
  }
}
