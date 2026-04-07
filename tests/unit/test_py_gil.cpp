#include "pyscript/py_gil.hpp"
#include "pyscript/py_interpreter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace atlas;

class PyGilTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
        {
            (void)PyInterpreter::initialize();
        }
    }
};

TEST_F(PyGilTest, GILGuardDoesNotDeadlock)
{
    // Main thread already holds GIL after initialize
    // GILGuard should acquire/release safely
    {
        GILGuard guard;
        // Can call Python API here
        auto py_val = PyObjectPtr(PyLong_FromLong(42));
        EXPECT_TRUE(py_val.is_int());
    }
    // After guard destroyed, GIL state should be restored
}

TEST_F(PyGilTest, GILReleaseAndReacquire)
{
    // Release GIL for "long C++ operation"
    {
        GILRelease release;
        // Cannot call Python API here (no GIL)
        // Simulate work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // GIL re-acquired, can call Python API
    auto py_val = PyObjectPtr(PyLong_FromLong(99));
    EXPECT_TRUE(py_val.is_int());
}

TEST_F(PyGilTest, WorkerThreadWithGILGuard)
{
    std::atomic<bool> thread_success{false};

    // Release GIL so worker thread can acquire it
    {
        GILRelease main_release;

        std::thread worker(
            [&]()
            {
                GILGuard guard;
                auto val = PyObjectPtr(PyLong_FromLong(123));
                thread_success = val.is_int();
            });
        worker.join();
    }

    EXPECT_TRUE(thread_success.load());
}

// ============================================================================
// Multi-thread GIL contention stress test
// ============================================================================

TEST_F(PyGilTest, MultipleThreadsContentForGIL)
{
    // N threads all compete for the GIL and perform a simple Python operation.
    // Success means: no crash, no deadlock, and all threads report success.
    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 50;

    std::atomic<int> success_count{0};

    // Release GIL from main thread so workers can acquire it freely.
    GILRelease main_release;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&]()
            {
                for (int i = 0; i < kItersPerThread; ++i)
                {
                    GILGuard guard;
                    // Simple Python integer round-trip to exercise the interpreter
                    auto obj = PyObjectPtr(PyLong_FromLong(i));
                    if (obj && obj.is_int())
                    {
                        success_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }

    for (auto& th : threads)
        th.join();

    EXPECT_EQ(success_count.load(), kThreads * kItersPerThread);
}

TEST_F(PyGilTest, GILReleaseAllowsParallelCppWork)
{
    // While main thread releases the GIL, two worker threads should be able to
    // proceed concurrently (each acquires the GIL for its own short window).
    constexpr int kThreads = 4;
    std::atomic<int> completed{0};

    {
        GILRelease main_release;

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back(
                [&]()
                {
                    GILGuard g;
                    auto v = PyObjectPtr(PyLong_FromLong(42));
                    if (v && v.is_int())
                    {
                        completed.fetch_add(1, std::memory_order_relaxed);
                    }
                });
        }
        for (auto& th : threads)
            th.join();
    }

    EXPECT_EQ(completed.load(), kThreads);
}

TEST_F(PyGilTest, NestedGILGuardIsIdempotent)
{
    // GILGuard::lock is re-entrant via PyGILState_Ensure — nesting must not deadlock.
    GILGuard outer;
    {
        GILGuard inner;
        auto v = PyObjectPtr(PyLong_FromLong(1));
        EXPECT_TRUE(v && v.is_int());
    }
    // outer still valid
    auto v2 = PyObjectPtr(PyLong_FromLong(2));
    EXPECT_TRUE(v2 && v2.is_int());
}
