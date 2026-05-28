#include <chrono>
#include <limits>

#include <gtest/gtest.h>

#include "movement_input_rate_limiter.h"

using namespace atlas;

TEST(MovementInputRateLimiter, ConsumesBurstAndRefillsOverTime) {
  MovementInputRateLimiter limiter(2.0, 2.0);
  const TimePoint now{};

  EXPECT_TRUE(limiter.Consume(42, now));
  EXPECT_TRUE(limiter.Consume(42, now));
  EXPECT_FALSE(limiter.Consume(42, now));

  EXPECT_TRUE(limiter.Consume(42, now + std::chrono::milliseconds(500)));
  EXPECT_FALSE(limiter.Consume(42, now + std::chrono::milliseconds(500)));
  EXPECT_TRUE(limiter.Consume(42, now + std::chrono::seconds(1)));
}

TEST(MovementInputRateLimiter, RejectsInvalidEntityWithoutConsumingBucket) {
  MovementInputRateLimiter limiter(1.0, 1.0);
  const TimePoint now{};

  EXPECT_FALSE(limiter.Consume(kInvalidEntityID, now));
  EXPECT_TRUE(limiter.Consume(42, now));
}

TEST(MovementInputRateLimiter, ClampsInvalidConfiguration) {
  MovementInputRateLimiter negative(-1.0, -1.0);
  MovementInputRateLimiter nonfinite(1.0, std::numeric_limits<double>::quiet_NaN());

  EXPECT_FALSE(negative.Consume(42, TimePoint{}));
  EXPECT_FALSE(nonfinite.Consume(42, TimePoint{}));
}

TEST(MovementInputRateLimiter, EraseResetsEntityBucket) {
  MovementInputRateLimiter limiter(1.0, 1.0);
  const TimePoint now{};

  EXPECT_TRUE(limiter.Consume(42, now));
  EXPECT_FALSE(limiter.Consume(42, now));

  limiter.Erase(42);

  EXPECT_TRUE(limiter.Consume(42, now));
}
