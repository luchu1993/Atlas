#include <chrono>

#include <gtest/gtest.h>

#include "coro/async_sleep.h"
#include "coro/cancellation.h"
#include "coro/fire_and_forget.h"
#include "network/event_dispatcher.h"

using namespace atlas;

class AsyncSleepTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }

  // Drive event loop until condition is met or deadline reached
  void drive_until(const std::atomic<bool>& done, Milliseconds timeout = Milliseconds(2000)) {
    auto deadline = Clock::now() + timeout;
    while (!done.load() && Clock::now() < deadline) dispatcher_.ProcessOnce();
  }
};

TEST_F(AsyncSleepTest, BasicSleep) {
  std::atomic<bool> completed{false};

  auto coro = [&]() -> FireAndForget {
    auto result = co_await async_sleep(dispatcher_, Milliseconds(10));
    EXPECT_TRUE(result.HasValue());
    completed = true;
  };
  coro();

  drive_until(completed);
  EXPECT_TRUE(completed.load());
}

TEST_F(AsyncSleepTest, Cancellation) {
  CancellationSource source;
  std::atomic<bool> completed{false};
  ErrorCode result_code = ErrorCode::kNone;

  auto coro = [&]() -> FireAndForget {
    auto result = co_await async_sleep(dispatcher_, Milliseconds(10000), source.Token());
    result_code = result ? ErrorCode::kNone : result.Error().Code();
    completed = true;
  };
  coro();

  // Cancel immediately
  source.RequestCancellation();
  drive_until(completed, Milliseconds(500));

  EXPECT_TRUE(completed.load());
  EXPECT_EQ(result_code, ErrorCode::kCancelled);
}

TEST_F(AsyncSleepTest, AlreadyCancelledToken) {
  CancellationSource source;
  source.RequestCancellation();

  std::atomic<bool> completed{false};
  ErrorCode result_code = ErrorCode::kNone;

  auto coro = [&]() -> FireAndForget {
    auto result = co_await async_sleep(dispatcher_, Milliseconds(10000), source.Token());
    result_code = result ? ErrorCode::kNone : result.Error().Code();
    completed = true;
  };
  coro();

  // Should complete immediately without entering event loop
  EXPECT_TRUE(completed.load());
  EXPECT_EQ(result_code, ErrorCode::kCancelled);
}
