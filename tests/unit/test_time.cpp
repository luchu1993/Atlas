#include <gtest/gtest.h>

#include "foundation/clock.h"

using namespace atlas;

TEST(GameClock, StartsAtZero) {
  GameClock clock;
  EXPECT_EQ(clock.Elapsed().count(), 0);
  EXPECT_EQ(clock.FrameCount(), 0u);
}

TEST(GameClock, TickAddsDelta) {
  GameClock clock;
  auto delta = Milliseconds(16);
  clock.Tick(delta);
  EXPECT_EQ(clock.Elapsed(), delta);
  EXPECT_EQ(clock.FrameCount(), 1u);

  clock.Tick(delta);
  EXPECT_EQ(clock.Elapsed(), Milliseconds(32));
  EXPECT_EQ(clock.FrameCount(), 2u);
}

TEST(GameClock, TimeScaleZeroPauses) {
  GameClock clock;
  clock.SetTimeScale(0.0);

  clock.Tick(Milliseconds(100));
  EXPECT_EQ(clock.Elapsed().count(), 0);
  EXPECT_EQ(clock.FrameCount(), 1u);  // frame still counts
}

TEST(GameClock, TimeScaleDoubles) {
  GameClock clock;
  clock.SetTimeScale(2.0);

  auto delta = Milliseconds(10);
  clock.Tick(delta);

  // With 2x scale, 10ms real -> 20ms game
  EXPECT_EQ(clock.Elapsed(), Milliseconds(20));
}

TEST(GameClock, Reset) {
  GameClock clock;
  clock.Tick(Milliseconds(50));
  clock.Tick(Milliseconds(50));
  EXPECT_EQ(clock.FrameCount(), 2u);

  clock.Reset();
  EXPECT_EQ(clock.Elapsed().count(), 0);
  EXPECT_EQ(clock.FrameCount(), 0u);
  EXPECT_DOUBLE_EQ(clock.TimeScale(), 1.0);
}
