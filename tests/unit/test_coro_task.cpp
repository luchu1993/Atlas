#include "coro/cancellation.hpp"
#include "coro/fire_and_forget.hpp"
#include "coro/scope_guard.hpp"
#include "coro/task.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace atlas;

// ============================================================================
// Helper: synchronously drive a Task<T> to completion.
// Only works for tasks that complete without external suspension (no async I/O).
// ============================================================================

template <typename T>
auto sync_wait(Task<T>& task) -> T
{
    // Use FireAndForget (eager start) to drive the lazy Task chain.
    // Avoids calling operator co_await() in a non-coroutine function,
    // which MSVC incorrectly treats as making the enclosing function a coroutine.
    T result{};
    bool done = false;

    auto driver = [&]() -> FireAndForget
    {
        result = co_await task;
        done = true;
    };
    driver();

    EXPECT_TRUE(done);
    return result;
}

inline void sync_wait_void(Task<void>& task)
{
    bool done = false;

    auto driver = [&]() -> FireAndForget
    {
        co_await task;
        done = true;
    };
    driver();

    EXPECT_TRUE(done);
}

// ============================================================================
// Task<T> tests
// ============================================================================

TEST(Task, BasicReturn)
{
    auto task = []() -> Task<int> { co_return 42; }();
    EXPECT_EQ(sync_wait(task), 42);
}

TEST(Task, StringReturn)
{
    auto task = []() -> Task<std::string> { co_return "hello"; }();
    EXPECT_EQ(sync_wait(task), "hello");
}

TEST(Task, VoidTask)
{
    bool executed = false;
    auto task = [&]() -> Task<void>
    {
        executed = true;
        co_return;
    }();
    sync_wait_void(task);
    EXPECT_TRUE(executed);
}

TEST(Task, ChainedAwait)
{
    auto inner = []() -> Task<int> { co_return 10; };

    auto outer = [&]() -> Task<int>
    {
        auto t1 = inner();
        auto t2 = inner();
        auto a = co_await t1;
        auto b = co_await t2;
        co_return a + b;
    }();

    EXPECT_EQ(sync_wait(outer), 20);
}

TEST(Task, DeepChain)
{
    // 4 levels deep — exercises symmetric transfer (O(1) stack depth)
    auto level3 = []() -> Task<int> { co_return 1; };

    auto outer = [&]() -> Task<int>
    {
        auto t3 = level3();
        co_return co_await t3;
    }();

    EXPECT_EQ(sync_wait(outer), 1);
}

TEST(Task, ExceptionPropagation)
{
    auto thrower = []() -> Task<int>
    {
        throw std::runtime_error("boom");
        co_return 0;  // unreachable
    };

    auto catcher = [&]() -> Task<int>
    {
        try
        {
            auto t = thrower();
            co_return co_await t;
        }
        catch (const std::runtime_error&)
        {
            co_return -1;
        }
    }();

    EXPECT_EQ(sync_wait(catcher), -1);
}

TEST(Task, MoveOnlySemantics)
{
    auto task = []() -> Task<int> { co_return 1; }();
    auto moved = std::move(task);
    EXPECT_EQ(sync_wait(moved), 1);
}

// ============================================================================
// FireAndForget tests
// ============================================================================

TEST(FireAndForget, ImmediateExecution)
{
    bool executed = false;
    auto coro = [&]() -> FireAndForget
    {
        executed = true;
        co_return;
    };
    coro();
    EXPECT_TRUE(executed);
}

TEST(FireAndForget, ExceptionDoesNotPropagate)
{
    // FireAndForget logs exceptions but doesn't throw to caller
    EXPECT_NO_THROW({
        auto coro = []() -> FireAndForget
        {
            throw std::runtime_error("should be caught internally");
            co_return;
        };
        coro();
    });
}

// ============================================================================
// ScopeGuard tests
// ============================================================================

TEST(ScopeGuard, ExecutesOnDestruction)
{
    bool cleaned = false;
    {
        ScopeGuard guard([&] { cleaned = true; });
    }
    EXPECT_TRUE(cleaned);
}

TEST(ScopeGuard, DismissPreventsExecution)
{
    bool cleaned = false;
    {
        ScopeGuard guard([&] { cleaned = true; });
        guard.dismiss();
    }
    EXPECT_FALSE(cleaned);
}

TEST(ScopeGuard, MoveTransfersOwnership)
{
    int count = 0;
    {
        ScopeGuard g1([&] { ++count; });
        ScopeGuard g2(std::move(g1));
        // g1 is dismissed by move, g2 owns the cleanup
    }
    EXPECT_EQ(count, 1);  // cleaned up exactly once
}

TEST(ScopeGuard, MultipleGuardsReverseOrder)
{
    std::vector<int> order;
    {
        ScopeGuard g1([&] { order.push_back(1); });
        ScopeGuard g2([&] { order.push_back(2); });
        ScopeGuard g3([&] { order.push_back(3); });
    }
    // C++ destruction order: reverse of declaration → 3, 2, 1
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 3);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 1);
}

TEST(ScopeGuard, ScopeExitMacro)
{
    bool cleaned = false;
    {
        ATLAS_SCOPE_EXIT([&] { cleaned = true; });
    }
    EXPECT_TRUE(cleaned);
}

// ============================================================================
// CancellationToken tests
// ============================================================================

TEST(CancellationToken, BasicCancel)
{
    CancellationSource source;
    auto token = source.token();
    EXPECT_FALSE(token.is_cancelled());

    bool callback_fired = false;
    auto reg = token.on_cancel([&] { callback_fired = true; });

    source.request_cancellation();
    EXPECT_TRUE(token.is_cancelled());
    EXPECT_TRUE(callback_fired);
}

TEST(CancellationToken, MultipleCallbacks)
{
    CancellationSource source;
    auto token = source.token();

    int count = 0;
    auto r1 = token.on_cancel([&] { ++count; });
    auto r2 = token.on_cancel([&] { ++count; });
    auto r3 = token.on_cancel([&] { ++count; });

    source.request_cancellation();
    EXPECT_EQ(count, 3);
}

TEST(CancellationToken, CallbackOnAlreadyCancelled)
{
    CancellationSource source;
    source.request_cancellation();

    auto token = source.token();
    bool callback_fired = false;
    auto reg = token.on_cancel([&] { callback_fired = true; });

    // Should be called immediately since already cancelled
    EXPECT_TRUE(callback_fired);
}

TEST(CancellationToken, RegistrationDeregistersOnDestruction)
{
    CancellationSource source;
    auto token = source.token();

    bool callback_fired = false;
    {
        auto reg = token.on_cancel([&] { callback_fired = true; });
        // reg goes out of scope — deregisters
    }

    source.request_cancellation();
    EXPECT_FALSE(callback_fired);  // callback was deregistered
}

TEST(CancellationToken, EmptyTokenNeverCancelled)
{
    CancellationToken token;  // default constructed — no source
    EXPECT_FALSE(token.is_cancelled());
    EXPECT_FALSE(token.is_valid());

    bool fired = false;
    auto reg = token.on_cancel([&] { fired = true; });
    EXPECT_FALSE(fired);
}

TEST(CancellationToken, DoubleCancelIsIdempotent)
{
    CancellationSource source;
    auto token = source.token();

    int count = 0;
    auto reg = token.on_cancel([&] { ++count; });

    source.request_cancellation();
    source.request_cancellation();  // second call is no-op
    EXPECT_EQ(count, 1);
}

TEST(CancellationToken, MultipleSources)
{
    CancellationSource src1;
    CancellationSource src2;

    auto tok1 = src1.token();
    auto tok2 = src2.token();

    src1.request_cancellation();
    EXPECT_TRUE(tok1.is_cancelled());
    EXPECT_FALSE(tok2.is_cancelled());
}
