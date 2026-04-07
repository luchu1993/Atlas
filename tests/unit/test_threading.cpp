#include "platform/threading.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace atlas;

TEST(ThreadPool, SubmitReturnsCorrectResult)
{
    ThreadPool pool(2);
    auto future = pool.submit([]() { return 42; });
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPool, MultipleTasksAllComplete)
{
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(10);

    for (int i = 0; i < 10; ++i)
    {
        futures.push_back(pool.submit([&counter]() { counter.fetch_add(1); }));
    }

    for (auto& f : futures)
    {
        f.get();
    }

    EXPECT_EQ(counter.load(), 10);
}

TEST(SpinLock, LockUnlockTryLock)
{
    SpinLock lock;

    lock.lock();
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(Threading, SetThreadNameDoesNotCrash)
{
    EXPECT_NO_THROW(set_thread_name("test_thread"));
}

// ============================================================================
// ThreadPool: lifecycle
// ============================================================================

TEST(ThreadPool, ConstructAndDestroyImmediately)
{
    // Constructing and destroying without submitting any tasks must not crash or hang.
    EXPECT_NO_THROW({ ThreadPool pool(2); });
}

TEST(ThreadPool, SingleThreadedPool)
{
    ThreadPool pool(1);
    auto f = pool.submit([] { return 7; });
    EXPECT_EQ(f.get(), 7);
}

TEST(ThreadPool, HardwareConcurrencyDefault)
{
    // Thread count 0 → hardware_concurrency, must be ≥ 1
    ThreadPool pool(0);
    EXPECT_GE(pool.thread_count(), 1u);
}

TEST(ThreadPool, OrderedResultsFromSingleThread)
{
    // With one thread, tasks execute in submission order.
    ThreadPool pool(1);
    std::vector<int> order;
    std::mutex m;

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i)
    {
        futures.push_back(pool.submit(
            [&order, &m, i]
            {
                std::lock_guard lock(m);
                order.push_back(i);
            }));
    }
    for (auto& f : futures)
        f.get();

    ASSERT_EQ(order.size(), 5u);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(order[i], i);
    }
}

TEST(ThreadPool, ShutdownDrainsPendingTasks)
{
    ThreadPool pool(2);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(20);

    for (int i = 0; i < 20; ++i)
    {
        futures.push_back(pool.submit([&counter] { counter.fetch_add(1); }));
    }

    // Wait for all futures before calling shutdown
    for (auto& f : futures)
        f.get();

    pool.shutdown();
    EXPECT_EQ(counter.load(), 20);
}

TEST(ThreadPool, ExceptionInTaskDoesNotCrashPool)
{
    ThreadPool pool(2);
    std::atomic<int> success_count{0};

    // Submit a task that throws, then several normal tasks
    auto bad = pool.submit([] { throw std::runtime_error("intentional"); });
    for (int i = 0; i < 5; ++i)
    {
        pool.submit([&success_count] { success_count.fetch_add(1); }).get();
    }

    // The bad future should rethrow the exception
    EXPECT_THROW(bad.get(), std::runtime_error);
    EXPECT_EQ(success_count.load(), 5);
}

// ============================================================================
// SpinLock: multi-thread contention
// ============================================================================

TEST(SpinLock, MultiThreadedMutualExclusion)
{
    SpinLock lock;
    int counter = 0;
    constexpr int kThreads = 4;
    constexpr int kIter = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < kIter; ++i)
                {
                    std::lock_guard<SpinLock> lg(lock);
                    ++counter;
                }
            });
    }
    for (auto& th : threads)
        th.join();

    EXPECT_EQ(counter, kThreads * kIter);
}

TEST(SpinLock, TryLockFailsWhenHeld)
{
    SpinLock lock;
    lock.lock();

    std::atomic<bool> try_result{true};
    std::thread t([&] { try_result = lock.try_lock(); });
    t.join();

    lock.unlock();
    EXPECT_FALSE(try_result.load());
}
