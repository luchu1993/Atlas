#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "foundation/memory_tracker.h"
#include "foundation/pool_allocator.h"

using namespace atlas;

TEST(PoolAllocator, AllocateAndDeallocate) {
  PoolAllocator pool("UnitTest.AllocAndDealloc", 64, 4);
  void* p = pool.Allocate();
  EXPECT_NE(p, nullptr);
  EXPECT_EQ(pool.BlocksInUse(), 1u);

  pool.Deallocate(p);
  EXPECT_EQ(pool.BlocksInUse(), 0u);
}

TEST(PoolAllocator, ReuseAfterDeallocation) {
  PoolAllocator pool("UnitTest.Reuse", 64, 4);
  void* p1 = pool.Allocate();
  pool.Deallocate(p1);

  void* p2 = pool.Allocate();
  EXPECT_NE(p2, nullptr);
  EXPECT_EQ(pool.BlocksInUse(), 1u);
  pool.Deallocate(p2);
  EXPECT_EQ(pool.BlocksInUse(), 0u);
}

TEST(PoolAllocator, GrowBeyondInitialCapacity) {
  PoolAllocator pool("UnitTest.Grow", 32, 2);  // only 2 initial blocks
  std::set<void*> ptrs;

  for (int i = 0; i < 10; ++i) {
    void* p = pool.Allocate();
    EXPECT_NE(p, nullptr);
    ptrs.insert(p);
  }

  EXPECT_EQ(ptrs.size(), 10u);  // all unique
  EXPECT_EQ(pool.BlocksInUse(), 10u);

  for (void* p : ptrs) {
    pool.Deallocate(p);
  }
  EXPECT_EQ(pool.BlocksInUse(), 0u);
}

TEST(TypedPool, ConstructAndDestroy) {
  struct Widget {
    std::string name;
    int value;
    Widget(std::string n, int v) : name(std::move(n)), value(v) {}
  };

  TypedPool<Widget> pool("UnitTest.Widget", 4);
  Widget* w = pool.Construct("test", 42);
  EXPECT_NE(w, nullptr);
  EXPECT_EQ(w->name, "test");
  EXPECT_EQ(w->value, 42);
  EXPECT_EQ(pool.BlocksInUse(), 1u);

  pool.Destroy(w);
  EXPECT_EQ(pool.BlocksInUse(), 0u);
}

TEST(MemoryTracker, RecordAllocDealloc) {
  auto& tracker = MemoryTracker::Instance();
  tracker.Reset();

  tracker.RecordAlloc(100);
  tracker.RecordAlloc(200);

  auto stats = tracker.GetStats();
  EXPECT_EQ(stats.current_bytes, 300u);
  EXPECT_EQ(stats.total_allocations, 2u);
  EXPECT_GE(stats.peak_bytes, 300u);

  tracker.RecordDealloc(100);
  stats = tracker.GetStats();
  EXPECT_EQ(stats.current_bytes, 200u);
  EXPECT_EQ(stats.total_deallocations, 1u);

  tracker.Reset();
}

// ============================================================================
// Review issue #9: PoolAllocator thread safety — allocate/deallocate are
// mutex-protected. Concurrent access should not crash or corrupt state.
// ============================================================================

TEST(PoolAllocator, ConcurrentAllocateAndDeallocate) {
  PoolAllocator pool("UnitTest.Concurrent", 64, 8);

  constexpr int kNumThreads = 4;
  constexpr int kOpsPerThread = 100;

  auto worker = [&]() {
    for (int i = 0; i < kOpsPerThread; ++i) {
      void* p = pool.Allocate();
      ASSERT_NE(p, nullptr);
      // Simulate brief use
      std::this_thread::yield();
      pool.Deallocate(p);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }

  // All blocks should have been returned
  EXPECT_EQ(pool.BlocksInUse(), 0u);
}

// ============================================================================
// BUG-07: TypedPool must allocate blocks that satisfy alignof(T).
// Without the fix, the Chunk header (8 bytes) shifts the first block to an
// 8-byte offset from a 16-byte-aligned malloc result, breaking alignas(16).
// ============================================================================

TEST(TypedPool, AllocatedPointersAreAligned) {
  struct alignas(16) Aligned16 {
    float v[4];
  };

  TypedPool<Aligned16> pool("UnitTest.Aligned16", 8);
  std::vector<Aligned16*> ptrs;

  for (int i = 0; i < 10; ++i) {
    auto* p = pool.Construct();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0u)
        << "pointer " << p << " is not 16-byte aligned";
    ptrs.push_back(p);
  }

  for (auto* p : ptrs) pool.Destroy(p);
}

// ============================================================================
// BUG-06: PoolAllocator::grow() must not throw; allocate() returns nullptr OOM.
// ============================================================================

TEST(PoolAllocator, GrowBeyondInitialCapacityDoesNotThrow) {
  // Start with 1 block; force multiple grows by allocating many more.
  PoolAllocator pool("UnitTest.GrowNoThrow", 32, 1);
  std::vector<void*> ptrs;

  EXPECT_NO_THROW({
    for (int i = 0; i < 64; ++i) {
      void* p = pool.Allocate();
      ASSERT_NE(p, nullptr);
      ptrs.push_back(p);
    }
  });

  EXPECT_EQ(pool.BlocksInUse(), 64u);
  for (void* p : ptrs) pool.Deallocate(p);
}
