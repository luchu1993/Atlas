#include <gtest/gtest.h>
#include "foundation/time.hpp"

using namespace atlas;

TEST(GameClock, StartsAtZero)
{
    GameClock clock;
    EXPECT_EQ(clock.elapsed().count(), 0);
    EXPECT_EQ(clock.frame_count(), 0u);
}

TEST(GameClock, TickAddsDelta)
{
    GameClock clock;
    auto delta = Milliseconds(16);
    clock.tick(delta);
    EXPECT_EQ(clock.elapsed(), delta);
    EXPECT_EQ(clock.frame_count(), 1u);

    clock.tick(delta);
    EXPECT_EQ(clock.elapsed(), Milliseconds(32));
    EXPECT_EQ(clock.frame_count(), 2u);
}

TEST(GameClock, TimeScaleZeroPauses)
{
    GameClock clock;
    clock.set_time_scale(0.0);

    clock.tick(Milliseconds(100));
    EXPECT_EQ(clock.elapsed().count(), 0);
    EXPECT_EQ(clock.frame_count(), 1u);  // frame still counts
}

TEST(GameClock, TimeScaleDoubles)
{
    GameClock clock;
    clock.set_time_scale(2.0);

    auto delta = Milliseconds(10);
    clock.tick(delta);

    // With 2x scale, 10ms real -> 20ms game
    EXPECT_EQ(clock.elapsed(), Milliseconds(20));
}

TEST(GameClock, Reset)
{
    GameClock clock;
    clock.tick(Milliseconds(50));
    clock.tick(Milliseconds(50));
    EXPECT_EQ(clock.frame_count(), 2u);

    clock.reset();
    EXPECT_EQ(clock.elapsed().count(), 0);
    EXPECT_EQ(clock.frame_count(), 0u);
    EXPECT_DOUBLE_EQ(clock.time_scale(), 1.0);
}
