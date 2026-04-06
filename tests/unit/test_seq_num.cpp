#include <gtest/gtest.h>
#include "network/seq_num.hpp"

#include <cstdint>
#include <limits>

using namespace atlas;

TEST(SeqNum, LessThanNormal)
{
    EXPECT_TRUE(seq_less_than(1, 2));
    EXPECT_TRUE(seq_less_than(100, 200));
    EXPECT_FALSE(seq_less_than(2, 1));
    EXPECT_FALSE(seq_less_than(5, 5));
}

TEST(SeqNum, LessThanWrapping)
{
    // Near uint32 boundary
    constexpr SeqNum near_max = std::numeric_limits<uint32_t>::max() - 5;
    constexpr SeqNum near_zero = 5;
    // near_max comes BEFORE near_zero in wrapping space
    EXPECT_TRUE(seq_less_than(near_max, near_zero));
    EXPECT_FALSE(seq_less_than(near_zero, near_max));
}

TEST(SeqNum, GreaterThanNormal)
{
    EXPECT_TRUE(seq_greater_than(10, 5));
    EXPECT_FALSE(seq_greater_than(5, 10));
    EXPECT_FALSE(seq_greater_than(5, 5));
}

TEST(SeqNum, GreaterThanWrapping)
{
    constexpr SeqNum near_max = std::numeric_limits<uint32_t>::max() - 2;
    constexpr SeqNum near_zero = 3;
    EXPECT_TRUE(seq_greater_than(near_zero, near_max));
}

TEST(SeqNum, DiffNormal)
{
    EXPECT_EQ(seq_diff(10, 5), 5);
    EXPECT_EQ(seq_diff(5, 10), -5);
}

TEST(SeqNum, DiffWrapping)
{
    constexpr SeqNum near_max = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(seq_diff(1, near_max), 2);  // 1 is 2 ahead of MAX
    EXPECT_EQ(seq_diff(near_max, 1), -2);
}

TEST(SeqNum, AckBitsSetAndTest)
{
    uint32_t bits = 0;
    SeqNum ack_num = 10;

    // Set bit for seq 9 (ack_num - 1 = diff 1, bit 0)
    ack_bits_set(bits, ack_num, 9);
    EXPECT_TRUE(ack_bits_test(bits, ack_num, 9));

    // Set bit for seq 7 (diff 3, bit 2)
    ack_bits_set(bits, ack_num, 7);
    EXPECT_TRUE(ack_bits_test(bits, ack_num, 7));

    // ack_num itself is always considered ACK'd
    EXPECT_TRUE(ack_bits_test(bits, ack_num, 10));

    // Seq 8 not set
    EXPECT_FALSE(ack_bits_test(bits, ack_num, 8));
}

TEST(SeqNum, AckBitsOutOfRange)
{
    uint32_t bits = 0xFFFFFFFF;
    SeqNum ack_num = 100;

    // Too old (diff > 32)
    EXPECT_FALSE(ack_bits_test(bits, ack_num, 50));

    // Future (seq > ack_num)
    EXPECT_FALSE(ack_bits_test(bits, ack_num, 101));
}

TEST(SeqNum, AckBitsFullWindow)
{
    uint32_t bits = 0;
    SeqNum ack_num = 50;

    // Set all 32 bits
    for (SeqNum s = 18; s < 50; ++s)
    {
        ack_bits_set(bits, ack_num, s);
    }

    // All should test true
    for (SeqNum s = 18; s <= 50; ++s)
    {
        EXPECT_TRUE(ack_bits_test(bits, ack_num, s)) << "Failed for seq=" << s;
    }
}
