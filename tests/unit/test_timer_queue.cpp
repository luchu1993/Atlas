#include <vector>

#include <gtest/gtest.h>

#include "foundation/timer_queue.h"

using namespace atlas;

class TimerQueueTest : public ::testing::Test {
 protected:
  TimerQueue queue;
  TimePoint base = Clock::now();
};

TEST_F(TimerQueueTest, OneShotFires) {
  int fired = 0;
  auto handle = queue.Schedule(base + Milliseconds(10), [&](TimerHandle) { ++fired; });
  EXPECT_TRUE(handle.IsValid());

  queue.Process(base + Milliseconds(10));
  EXPECT_EQ(fired, 1);
}

TEST_F(TimerQueueTest, OneShotDoesNotFireEarly) {
  int fired = 0;
  auto handle = queue.Schedule(base + Milliseconds(100), [&](TimerHandle) { ++fired; });
  (void)handle;

  queue.Process(base);
  EXPECT_EQ(fired, 0);
}

TEST_F(TimerQueueTest, RepeatingFiresMultipleTimes) {
  int count = 0;
  auto handle = queue.ScheduleRepeating(base, Milliseconds(10), [&](TimerHandle) { ++count; });
  (void)handle;

  queue.Process(base);                     // fires at base
  queue.Process(base + Milliseconds(10));  // fires at base+10
  queue.Process(base + Milliseconds(20));  // fires at base+20
  EXPECT_EQ(count, 3);
}

TEST_F(TimerQueueTest, CancelBeforeFire) {
  int fired = 0;
  auto handle = queue.Schedule(base + Milliseconds(10), [&](TimerHandle) { ++fired; });

  EXPECT_TRUE(queue.Cancel(handle));
  queue.Process(base + Milliseconds(100));
  EXPECT_EQ(fired, 0);
}

TEST_F(TimerQueueTest, TimeUntilNext) {
  auto handle = queue.Schedule(base + Milliseconds(50), [](TimerHandle) {});
  (void)handle;

  auto dt = queue.TimeUntilNext(base);
  EXPECT_EQ(dt, Milliseconds(50));
}

TEST_F(TimerQueueTest, MultipleTimersFireInOrder) {
  std::vector<int> order;
  auto h1 = queue.Schedule(base + Milliseconds(30), [&](TimerHandle) { order.push_back(3); });
  auto h2 = queue.Schedule(base + Milliseconds(10), [&](TimerHandle) { order.push_back(1); });
  auto h3 = queue.Schedule(base + Milliseconds(20), [&](TimerHandle) { order.push_back(2); });
  (void)h1;
  (void)h2;
  (void)h3;

  queue.Process(base + Milliseconds(50));

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST_F(TimerQueueTest, EmptyQueueMaxDuration) {
  auto dt = queue.TimeUntilNext(base);
  EXPECT_EQ(dt, Duration::max());
}

// ============================================================================
// Review issue: callback exception safety
// ============================================================================

TEST_F(TimerQueueTest, CallbackExceptionDoesNotLeak) {
  int normal_fired = 0;
  auto h1 = queue.Schedule(base + Milliseconds(10),
                           [&](TimerHandle) { throw std::runtime_error("boom"); });
  auto h2 = queue.Schedule(base + Milliseconds(20), [&](TimerHandle) { ++normal_fired; });
  (void)h1;
  (void)h2;

  // First timer throws — safe_invoke catches it; process() must not propagate
  // and the queue must remain functional for subsequent timers.
  EXPECT_NO_THROW(queue.Process(base + Milliseconds(15)));

  // Second timer should still be in the queue and fire normally
  queue.Process(base + Milliseconds(25));
  EXPECT_EQ(normal_fired, 1);
}

// ============================================================================
// Review issue: cancel invalid handle
// ============================================================================

TEST_F(TimerQueueTest, CancelInvalidHandle) {
  TimerHandle invalid;
  EXPECT_FALSE(queue.Cancel(invalid));

  TimerHandle made_up{};
  EXPECT_FALSE(queue.Cancel(made_up));
}

// ============================================================================
// Review issue: cancel already-fired one-shot timer
// ============================================================================

TEST_F(TimerQueueTest, CancelAlreadyFiredOneShot) {
  int fired = 0;
  auto handle = queue.Schedule(base, [&](TimerHandle) { ++fired; });
  queue.Process(base);
  EXPECT_EQ(fired, 1);

  // Timer already fired and removed — cancel should return false
  EXPECT_FALSE(queue.Cancel(handle));
}

// ============================================================================
// Review issue: many timers performance
// ============================================================================

TEST_F(TimerQueueTest, ManyTimersCorrectOrder) {
  std::vector<int> order;
  constexpr int N = 100;
  for (int i = N - 1; i >= 0; --i) {
    auto h = queue.Schedule(base + Milliseconds(i), [&, i](TimerHandle) { order.push_back(i); });
    (void)h;
  }

  queue.Process(base + Milliseconds(N));
  ASSERT_EQ(static_cast<int>(order.size()), N);
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(order[i], i);
  }
}

// ============================================================================
// Review issue #3: TimerQueue reentrancy safety — schedule() during process()
// callback is safe because pop-before-callback + push_heap maintains heap
// invariant.
// ============================================================================

TEST_F(TimerQueueTest, ScheduleDuringProcessCallback) {
  int first_fired = 0;
  int nested_fired = 0;

  auto handle = queue.Schedule(base, [&](TimerHandle) {
    ++first_fired;
    // Schedule a new timer from within the callback that fires
    // immediately
    auto nested = queue.Schedule(base, [&](TimerHandle) { ++nested_fired; });
    (void)nested;
  });
  (void)handle;

  // First process: fires the original timer, which schedules the nested one
  queue.Process(base);
  EXPECT_EQ(first_fired, 1);

  // The nested timer was scheduled during process(). It may or may not fire
  // in the same process() call depending on implementation. Either way, a
  // second process() call should ensure it fires.
  queue.Process(base);
  EXPECT_EQ(nested_fired, 1);
}

// ============================================================================
// BUG-05: time_until_next() must skip cancelled heap-top nodes and return the
// deadline of the next *valid* timer, not Duration::max().
// ============================================================================

TEST_F(TimerQueueTest, TimeUntilNextSkipsCancelledFront) {
  // A fires first (t+10), B fires second (t+50)
  auto ha = queue.Schedule(base + Milliseconds(10), [](TimerHandle) {});
  auto hb = queue.Schedule(base + Milliseconds(50), [](TimerHandle) {});
  (void)hb;

  // Cancel A — it was the heap front.  B should now be the effective front.
  queue.Cancel(ha);

  // time_until_next() must reflect B (50 ms), not return Duration::max().
  auto dt = queue.TimeUntilNext(base);
  EXPECT_EQ(dt, Milliseconds(50));
}

TEST_F(TimerQueueTest, TimeUntilNextAfterAllCancelledIsMax) {
  auto ha = queue.Schedule(base + Milliseconds(10), [](TimerHandle) {});
  auto hb = queue.Schedule(base + Milliseconds(20), [](TimerHandle) {});

  queue.Cancel(ha);
  queue.Cancel(hb);

  // All timers cancelled — must return max.
  EXPECT_EQ(queue.TimeUntilNext(base), Duration::max());
  EXPECT_TRUE(queue.empty());
}

// ============================================================================
// Review issue #4: TimerQueue cancel during process — cancel() during
// process() callback sets cancelled flag so second timer does NOT fire.
// ============================================================================

TEST_F(TimerQueueTest, CancelOtherTimerDuringProcessCallback) {
  int first_fired = 0;
  int second_fired = 0;
  TimerHandle second_handle;

  // Schedule two timers at the same time. The first cancels the second.
  auto h1 = queue.Schedule(base, [&](TimerHandle) {
    ++first_fired;
    queue.Cancel(second_handle);
  });
  (void)h1;

  second_handle = queue.Schedule(base, [&](TimerHandle) { ++second_fired; });

  // Process both — the first should fire and cancel the second
  queue.Process(base);

  EXPECT_EQ(first_fired, 1);
  EXPECT_EQ(second_fired, 0);  // cancelled by first timer's callback
}
