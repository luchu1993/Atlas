#include <gtest/gtest.h>
#include "foundation/timer_queue.hpp"

#include <vector>

using namespace atlas;

class TimerQueueTest : public ::testing::Test
{
protected:
    TimerQueue queue;
    TimePoint base = Clock::now();
};

TEST_F(TimerQueueTest, OneShotFires)
{
    int fired = 0;
    auto handle = queue.schedule(base + Milliseconds(10), [&](TimerHandle) { ++fired; });
    EXPECT_TRUE(handle.is_valid());

    queue.process(base + Milliseconds(10));
    EXPECT_EQ(fired, 1);
}

TEST_F(TimerQueueTest, OneShotDoesNotFireEarly)
{
    int fired = 0;
    auto handle = queue.schedule(base + Milliseconds(100), [&](TimerHandle) { ++fired; });
    (void)handle;

    queue.process(base);
    EXPECT_EQ(fired, 0);
}

TEST_F(TimerQueueTest, RepeatingFiresMultipleTimes)
{
    int count = 0;
    auto handle = queue.schedule_repeating(base, Milliseconds(10), [&](TimerHandle) { ++count; });
    (void)handle;

    queue.process(base);                     // fires at base
    queue.process(base + Milliseconds(10));  // fires at base+10
    queue.process(base + Milliseconds(20));  // fires at base+20
    EXPECT_EQ(count, 3);
}

TEST_F(TimerQueueTest, CancelBeforeFire)
{
    int fired = 0;
    auto handle = queue.schedule(base + Milliseconds(10), [&](TimerHandle) { ++fired; });

    EXPECT_TRUE(queue.cancel(handle));
    queue.process(base + Milliseconds(100));
    EXPECT_EQ(fired, 0);
}

TEST_F(TimerQueueTest, TimeUntilNext)
{
    auto handle = queue.schedule(base + Milliseconds(50), [](TimerHandle) {});
    (void)handle;

    auto dt = queue.time_until_next(base);
    EXPECT_EQ(dt, Milliseconds(50));
}

TEST_F(TimerQueueTest, MultipleTimersFireInOrder)
{
    std::vector<int> order;
    auto h1 = queue.schedule(base + Milliseconds(30), [&](TimerHandle) { order.push_back(3); });
    auto h2 = queue.schedule(base + Milliseconds(10), [&](TimerHandle) { order.push_back(1); });
    auto h3 = queue.schedule(base + Milliseconds(20), [&](TimerHandle) { order.push_back(2); });
    (void)h1; (void)h2; (void)h3;

    queue.process(base + Milliseconds(50));

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST_F(TimerQueueTest, EmptyQueueMaxDuration)
{
    auto dt = queue.time_until_next(base);
    EXPECT_EQ(dt, Duration::max());
}
