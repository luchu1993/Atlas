#include "foundation/memory_tracker.hpp"
#include "foundation/pool_allocator.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace atlas;

TEST(PoolAllocator, AllocateAndDeallocate)
{
    PoolAllocator pool(64, 4);
    void* p = pool.allocate();
    EXPECT_NE(p, nullptr);
    EXPECT_EQ(pool.blocks_in_use(), 1u);

    pool.deallocate(p);
    EXPECT_EQ(pool.blocks_in_use(), 0u);
}

TEST(PoolAllocator, ReuseAfterDeallocation)
{
    PoolAllocator pool(64, 4);
    void* p1 = pool.allocate();
    pool.deallocate(p1);

    void* p2 = pool.allocate();
    EXPECT_NE(p2, nullptr);
    EXPECT_EQ(pool.blocks_in_use(), 1u);
    pool.deallocate(p2);
    EXPECT_EQ(pool.blocks_in_use(), 0u);
}

TEST(PoolAllocator, GrowBeyondInitialCapacity)
{
    PoolAllocator pool(32, 2);  // only 2 initial blocks
    std::set<void*> ptrs;

    for (int i = 0; i < 10; ++i)
    {
        void* p = pool.allocate();
        EXPECT_NE(p, nullptr);
        ptrs.insert(p);
    }

    EXPECT_EQ(ptrs.size(), 10u);  // all unique
    EXPECT_EQ(pool.blocks_in_use(), 10u);

    for (void* p : ptrs)
    {
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.blocks_in_use(), 0u);
}

TEST(TypedPool, ConstructAndDestroy)
{
    struct Widget
    {
        std::string name;
        int value;
        Widget(std::string n, int v) : name(std::move(n)), value(v) {}
    };

    TypedPool<Widget> pool(4);
    Widget* w = pool.construct("test", 42);
    EXPECT_NE(w, nullptr);
    EXPECT_EQ(w->name, "test");
    EXPECT_EQ(w->value, 42);
    EXPECT_EQ(pool.blocks_in_use(), 1u);

    pool.destroy(w);
    EXPECT_EQ(pool.blocks_in_use(), 0u);
}

TEST(MemoryTracker, RecordAllocDealloc)
{
    auto& tracker = MemoryTracker::instance();
    tracker.reset();

    tracker.record_alloc(100);
    tracker.record_alloc(200);

    auto stats = tracker.stats();
    EXPECT_EQ(stats.current_bytes, 300u);
    EXPECT_EQ(stats.total_allocations, 2u);
    EXPECT_GE(stats.peak_bytes, 300u);

    tracker.record_dealloc(100);
    stats = tracker.stats();
    EXPECT_EQ(stats.current_bytes, 200u);
    EXPECT_EQ(stats.total_deallocations, 1u);

    tracker.reset();
}

// ============================================================================
// Review issue #9: PoolAllocator thread safety — allocate/deallocate are
// mutex-protected. Concurrent access should not crash or corrupt state.
// ============================================================================

TEST(PoolAllocator, ConcurrentAllocateAndDeallocate)
{
    PoolAllocator pool(64, 8);

    constexpr int kNumThreads = 4;
    constexpr int kOpsPerThread = 100;

    auto worker = [&]()
    {
        for (int i = 0; i < kOpsPerThread; ++i)
        {
            void* p = pool.allocate();
            ASSERT_NE(p, nullptr);
            // Simulate brief use
            std::this_thread::yield();
            pool.deallocate(p);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (int i = 0; i < kNumThreads; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto& t : threads)
    {
        t.join();
    }

    // All blocks should have been returned
    EXPECT_EQ(pool.blocks_in_use(), 0u);
}
