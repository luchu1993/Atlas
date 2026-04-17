#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "foundation/containers/byte_ring_buffer.h"
#include "foundation/containers/flat_map.h"
#include "foundation/containers/object_pool.h"
#include "foundation/containers/paged_sparse_table.h"
#include "foundation/containers/ring_buffer.h"
#include "foundation/containers/slot_map.h"

using namespace atlas;

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

TEST(RingBuffer, PushUntilFullPopAllFIFO) {
  RingBuffer<int> rb(4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(rb.PushBack(i));
  }
  EXPECT_TRUE(rb.Full());
  EXPECT_FALSE(rb.PushBack(99));  // full

  for (int i = 0; i < 4; ++i) {
    auto val = rb.PopFront();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }
  EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, WrapAround) {
  RingBuffer<int> rb(3);
  rb.PushBack(1);
  rb.PushBack(2);
  rb.PopFront();  // remove 1, head advances
  rb.PushBack(3);
  rb.PushBack(4);  // wraps around

  EXPECT_EQ(rb.size(), 3u);
  EXPECT_EQ(*rb.PopFront(), 2);
  EXPECT_EQ(*rb.PopFront(), 3);
  EXPECT_EQ(*rb.PopFront(), 4);
}

TEST(RingBuffer, IndexAccess) {
  RingBuffer<int> rb(4);
  rb.PushBack(10);
  rb.PushBack(20);
  rb.PushBack(30);

  EXPECT_EQ(rb[0], 10);
  EXPECT_EQ(rb[1], 20);
  EXPECT_EQ(rb[2], 30);
}

// ---------------------------------------------------------------------------
// ByteRingBuffer
// ---------------------------------------------------------------------------

TEST(ByteRingBuffer, EnsureWritableGrowsWithinConfiguredLimit) {
  ByteRingBuffer rb(8, 64);
  EXPECT_EQ(rb.Capacity(), 8u);
  EXPECT_EQ(rb.MinCapacity(), 8u);
  EXPECT_EQ(rb.MaxCapacity(), 64u);

  const std::array<std::byte, 4> payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
                                         std::byte{'d'}};
  EXPECT_TRUE(rb.Append(payload));
  EXPECT_TRUE(rb.EnsureWritable(20));
  EXPECT_EQ(rb.Capacity(), 32u);
  EXPECT_FALSE(rb.EnsureWritable(61));
}

TEST(ByteRingBuffer, PeekFrontHandlesWrappedReadableData) {
  ByteRingBuffer rb(8, 32);

  const std::array<std::byte, 6> first{std::byte{1}, std::byte{2}, std::byte{3},
                                       std::byte{4}, std::byte{5}, std::byte{6}};
  EXPECT_TRUE(rb.Append(first));
  rb.Consume(4);

  const std::array<std::byte, 4> second{std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
  EXPECT_TRUE(rb.Append(second));

  std::array<std::byte, 6> out{};
  ASSERT_TRUE(rb.PeekFront(out));
  EXPECT_EQ(out[0], std::byte{5});
  EXPECT_EQ(out[1], std::byte{6});
  EXPECT_EQ(out[2], std::byte{7});
  EXPECT_EQ(out[3], std::byte{8});
  EXPECT_EQ(out[4], std::byte{9});
  EXPECT_EQ(out[5], std::byte{10});
}

TEST(ByteRingBuffer, ShrinkToFitReturnsToBaselineAfterBurst) {
  ByteRingBuffer rb(8, 64);
  EXPECT_TRUE(rb.EnsureWritable(40));
  EXPECT_EQ(rb.Capacity(), 64u);

  const std::array<std::byte, 40> payload{};
  EXPECT_TRUE(rb.Append(payload));
  rb.Consume(payload.size());

  rb.ShrinkToFit();
  EXPECT_EQ(rb.Capacity(), 8u);
  EXPECT_EQ(rb.ReadableSize(), 0u);
  EXPECT_EQ(rb.WritableSize(), 8u);
}

// ---------------------------------------------------------------------------
// SlotMap
// ---------------------------------------------------------------------------

TEST(SlotMap, InsertGetRemove) {
  SlotMap<std::string> map;
  auto h = map.Insert("hello");
  EXPECT_TRUE(h.IsValid());
  EXPECT_EQ(map.size(), 1u);

  auto* val = map.Get(h);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(*val, "hello");

  EXPECT_TRUE(map.Remove(h));
  EXPECT_EQ(map.size(), 0u);
}

TEST(SlotMap, HandleInvalidationAfterRemove) {
  SlotMap<int> map;
  auto h = map.Insert(42);
  map.Remove(h);

  // Old handle should be invalid due to generation bump
  EXPECT_FALSE(map.Contains(h));
  EXPECT_EQ(map.Get(h), nullptr);
}

TEST(SlotMap, DenseIterationNoGaps) {
  SlotMap<int> map;
  auto h1 = map.Insert(1);
  map.Insert(2);
  map.Insert(3);
  map.Remove(h1);  // remove first element

  std::vector<int> values;
  for (auto& v : map) {
    values.push_back(v);
  }

  EXPECT_EQ(values.size(), 2u);
  // Values 2 and 3 should be present (order may differ due to swap-remove)
  std::sort(values.begin(), values.end());
  EXPECT_EQ(values[0], 2);
  EXPECT_EQ(values[1], 3);
}

// ---------------------------------------------------------------------------
// ObjectPool
// ---------------------------------------------------------------------------

TEST(ObjectPool, CreateGetDestroyIsValid) {
  ObjectPool<int> pool;
  auto h = pool.Create(42);
  EXPECT_TRUE(pool.IsValid(h));

  auto* val = pool.Get(h);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(*val, 42);

  pool.Destroy(h);
  EXPECT_FALSE(pool.IsValid(h));
  EXPECT_EQ(pool.Get(h), nullptr);
}

// ---------------------------------------------------------------------------
// FlatMap
// ---------------------------------------------------------------------------

TEST(FlatMap, InsertAndFind) {
  FlatMap<int, std::string> map;
  auto [it, inserted] = map.Insert(1, "one");
  EXPECT_TRUE(inserted);
  EXPECT_EQ(it->second, "one");

  auto found = map.Find(1);
  ASSERT_NE(found, map.end());
  EXPECT_EQ(found->second, "one");

  EXPECT_EQ(map.Find(99), map.end());
}

TEST(FlatMap, OperatorBracket) {
  FlatMap<std::string, int> map;
  map["x"] = 10;
  map["y"] = 20;
  EXPECT_EQ(map["x"], 10);
  EXPECT_EQ(map["y"], 20);
}

TEST(FlatMap, Erase) {
  FlatMap<int, int> map;
  map.Insert(1, 100);
  map.Insert(2, 200);
  EXPECT_TRUE(map.Erase(1));
  EXPECT_FALSE(map.Contains(1));
  EXPECT_TRUE(map.Contains(2));
  EXPECT_FALSE(map.Erase(99));
}

TEST(FlatMap, MaintainsSortedOrder) {
  FlatMap<int, int> map;
  map.Insert(3, 30);
  map.Insert(1, 10);
  map.Insert(2, 20);

  std::vector<int> keys;
  for (auto& [k, v] : map) {
    keys.push_back(k);
  }
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST(FlatMap, ContainsEmptySize) {
  FlatMap<int, int> map;
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(map.size(), 0u);
  EXPECT_FALSE(map.Contains(1));

  map.Insert(1, 10);
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(map.size(), 1u);
  EXPECT_TRUE(map.Contains(1));
}

// ============================================================================
// Review issue: RingBuffer pop empty returns nullopt
// ============================================================================

TEST(RingBuffer, PopEmptyReturnsNullopt) {
  RingBuffer<int> rb(4);
  auto val = rb.PopFront();
  EXPECT_FALSE(val.has_value());
}

TEST(RingBuffer, ClearResetsState) {
  RingBuffer<int> rb(4);
  rb.PushBack(1);
  rb.PushBack(2);
  rb.Clear();
  EXPECT_TRUE(rb.empty());
  EXPECT_EQ(rb.size(), 0u);

  // Should be able to push again after clear
  EXPECT_TRUE(rb.PushBack(3));
  EXPECT_EQ(*rb.PopFront(), 3);
}

// ============================================================================
// Review issue: SlotMap generation bump on remove
// ============================================================================

TEST(SlotMap, SlotReuseWithNewGeneration) {
  SlotMap<int> map;
  auto h1 = map.Insert(100);
  map.Remove(h1);

  // Insert again — reuses the same slot index but with bumped generation
  auto h2 = map.Insert(200);
  EXPECT_TRUE(h2.IsValid());
  EXPECT_NE(h1.generation, h2.generation);  // generation should differ

  // Old handle should not work
  EXPECT_FALSE(map.Contains(h1));
  EXPECT_EQ(map.Get(h1), nullptr);

  // New handle should work
  EXPECT_TRUE(map.Contains(h2));
  EXPECT_EQ(*map.Get(h2), 200);
}

TEST(SlotMap, ClearInvalidatesAllHandles) {
  SlotMap<int> map;
  auto h1 = map.Insert(1);
  auto h2 = map.Insert(2);
  auto h3 = map.Insert(3);

  map.Clear();
  EXPECT_EQ(map.size(), 0u);
  EXPECT_FALSE(map.Contains(h1));
  EXPECT_FALSE(map.Contains(h2));
  EXPECT_FALSE(map.Contains(h3));

  // Can insert again after clear
  auto h4 = map.Insert(4);
  EXPECT_TRUE(map.Contains(h4));
  EXPECT_EQ(*map.Get(h4), 4);
}

// ============================================================================
// Review issue: FlatMap duplicate insert
// ============================================================================

TEST(FlatMap, DuplicateInsertReturnsFalse) {
  FlatMap<int, std::string> map;
  auto [it1, ok1] = map.Insert(1, "first");
  EXPECT_TRUE(ok1);

  auto [it2, ok2] = map.Insert(1, "second");
  EXPECT_FALSE(ok2);
  EXPECT_EQ(it2->second, "first");  // original value preserved
}

TEST(FlatMap, InsertOrAssignUpdatesExisting) {
  FlatMap<int, std::string> map;
  map.InsertOrAssign(1, "first");
  map.InsertOrAssign(1, "updated");
  EXPECT_EQ(map.Find(1)->second, "updated");
}

// ============================================================================
// Review issue: ObjectPool dense iteration after multiple create/destroy
// ============================================================================

TEST(ObjectPool, IterationAfterMixedCreateDestroy) {
  ObjectPool<int> pool;
  [[maybe_unused]] auto h1 = pool.Create(10);
  auto h2 = pool.Create(20);
  auto h3 = pool.Create(30);
  [[maybe_unused]] auto h4 = pool.Create(40);

  pool.Destroy(h2);
  pool.Destroy(h3);

  std::vector<int> values;
  for (auto& v : pool) {
    values.push_back(v);
  }
  EXPECT_EQ(values.size(), 2u);
  std::sort(values.begin(), values.end());
  EXPECT_EQ(values[0], 10);
  EXPECT_EQ(values[1], 40);
}

// ---------------------------------------------------------------------------
// PagedSparseTable
// ---------------------------------------------------------------------------

TEST(PagedSparseTable, AllocatesPagesLazily) {
  PagedSparseTable<uint16_t, int> table;
  EXPECT_EQ(table.size(), 0u);
  EXPECT_EQ(table.AllocatedPageCount(), 0u);

  EXPECT_TRUE(table.Insert(42, std::make_unique<int>(7)));
  EXPECT_EQ(table.size(), 1u);
  EXPECT_EQ(table.AllocatedPageCount(), 1u);
  ASSERT_NE(table.Get(42), nullptr);
  EXPECT_EQ(*table.Get(42), 7);
}

TEST(PagedSparseTable, SeparatePagesStaySparse) {
  PagedSparseTable<uint16_t, int> table;
  EXPECT_TRUE(table.Insert(0x0001, std::make_unique<int>(1)));
  EXPECT_TRUE(table.Insert(0x1202, std::make_unique<int>(2)));
  EXPECT_TRUE(table.Insert(0xFF03, std::make_unique<int>(3)));

  EXPECT_EQ(table.size(), 3u);
  EXPECT_EQ(table.AllocatedPageCount(), 3u);
  EXPECT_EQ(*table.Get(0x0001), 1);
  EXPECT_EQ(*table.Get(0x1202), 2);
  EXPECT_EQ(*table.Get(0xFF03), 3);
  EXPECT_EQ(table.Get(0x1203), nullptr);
}

TEST(PagedSparseTable, DuplicateInsertRejectedAndEraseUpdatesSize) {
  PagedSparseTable<uint16_t, int> table;
  EXPECT_TRUE(table.Insert(5000, std::make_unique<int>(11)));
  EXPECT_FALSE(table.Insert(5000, std::make_unique<int>(22)));
  ASSERT_NE(table.Get(5000), nullptr);
  EXPECT_EQ(*table.Get(5000), 11);

  EXPECT_TRUE(table.Erase(5000));
  EXPECT_FALSE(table.Erase(5000));
  EXPECT_EQ(table.size(), 0u);
  EXPECT_EQ(table.Get(5000), nullptr);
}

TEST(PagedSparseTable, ClearReleasesAllPages) {
  PagedSparseTable<uint16_t, int> table;
  EXPECT_TRUE(table.Insert(1, std::make_unique<int>(1)));
  EXPECT_TRUE(table.Insert(0x2201, std::make_unique<int>(2)));
  EXPECT_EQ(table.AllocatedPageCount(), 2u);

  table.Clear();
  EXPECT_EQ(table.size(), 0u);
  EXPECT_EQ(table.AllocatedPageCount(), 0u);
  EXPECT_EQ(table.Get(1), nullptr);
  EXPECT_EQ(table.Get(0x2201), nullptr);
}
