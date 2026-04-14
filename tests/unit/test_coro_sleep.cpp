#include "coro/async_sleep.hpp"
#include "coro/cancellation.hpp"
#include "coro/fire_and_forget.hpp"
#include "network/event_dispatcher.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace atlas;

class AsyncSleepTest : public ::testing::Test
{
protected:
    EventDispatcher dispatcher_{"test"};

    void SetUp() override { dispatcher_.set_max_poll_wait(Milliseconds(1)); }

    // Drive event loop until condition is met or deadline reached
    void drive_until(const std::atomic<bool>& done, Milliseconds timeout = Milliseconds(2000))
    {
        auto deadline = Clock::now() + timeout;
        while (!done.load() && Clock::now() < deadline)
            dispatcher_.process_once();
    }
};

TEST_F(AsyncSleepTest, BasicSleep)
{
    std::atomic<bool> completed{false};

    auto coro = [&]() -> FireAndForget
    {
        auto result = co_await async_sleep(dispatcher_, Milliseconds(10));
        EXPECT_TRUE(result.has_value());
        completed = true;
    };
    coro();

    drive_until(completed);
    EXPECT_TRUE(completed.load());
}

TEST_F(AsyncSleepTest, Cancellation)
{
    CancellationSource source;
    std::atomic<bool> completed{false};
    ErrorCode result_code = ErrorCode::None;

    auto coro = [&]() -> FireAndForget
    {
        auto result = co_await async_sleep(dispatcher_, Milliseconds(10000), source.token());
        result_code = result ? ErrorCode::None : result.error().code();
        completed = true;
    };
    coro();

    // Cancel immediately
    source.request_cancellation();
    drive_until(completed, Milliseconds(500));

    EXPECT_TRUE(completed.load());
    EXPECT_EQ(result_code, ErrorCode::Cancelled);
}

TEST_F(AsyncSleepTest, AlreadyCancelledToken)
{
    CancellationSource source;
    source.request_cancellation();

    std::atomic<bool> completed{false};
    ErrorCode result_code = ErrorCode::None;

    auto coro = [&]() -> FireAndForget
    {
        auto result = co_await async_sleep(dispatcher_, Milliseconds(10000), source.token());
        result_code = result ? ErrorCode::None : result.error().code();
        completed = true;
    };
    coro();

    // Should complete immediately without entering event loop
    EXPECT_TRUE(completed.load());
    EXPECT_EQ(result_code, ErrorCode::Cancelled);
}
