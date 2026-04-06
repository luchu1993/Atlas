#include <gtest/gtest.h>
#include "platform/threading.hpp"

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
