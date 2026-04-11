#include "foundation/containers/byte_ring_buffer.hpp"
#include "foundation/containers/flat_map.hpp"
#include "foundation/containers/object_pool.hpp"
#include "foundation/containers/paged_sparse_table.hpp"
#include "foundation/containers/ring_buffer.hpp"
#include "foundation/containers/slot_map.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace atlas;

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

TEST(RingBuffer, PushUntilFullPopAllFIFO)
{
    RingBuffer<int> rb(4);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_TRUE(rb.push_back(i));
    }
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.push_back(99));  // full

    for (int i = 0; i < 4; ++i)
    {
        auto val = rb.pop_front();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, i);
    }
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, WrapAround)
{
    RingBuffer<int> rb(3);
    rb.push_back(1);
    rb.push_back(2);
    rb.pop_front();  // remove 1, head advances
    rb.push_back(3);
    rb.push_back(4);  // wraps around

    EXPECT_EQ(rb.size(), 3u);
    EXPECT_EQ(*rb.pop_front(), 2);
    EXPECT_EQ(*rb.pop_front(), 3);
    EXPECT_EQ(*rb.pop_front(), 4);
}

TEST(RingBuffer, IndexAccess)
{
    RingBuffer<int> rb(4);
    rb.push_back(10);
    rb.push_back(20);
    rb.push_back(30);

    EXPECT_EQ(rb[0], 10);
    EXPECT_EQ(rb[1], 20);
    EXPECT_EQ(rb[2], 30);
}

// ---------------------------------------------------------------------------
// ByteRingBuffer
// ---------------------------------------------------------------------------

TEST(ByteRingBuffer, EnsureWritableGrowsWithinConfiguredLimit)
{
    ByteRingBuffer rb(8, 64);
    EXPECT_EQ(rb.capacity(), 8u);
    EXPECT_EQ(rb.min_capacity(), 8u);
    EXPECT_EQ(rb.max_capacity(), 64u);

    const std::array<std::byte, 4> payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
                                           std::byte{'d'}};
    EXPECT_TRUE(rb.append(payload));
    EXPECT_TRUE(rb.ensure_writable(20));
    EXPECT_EQ(rb.capacity(), 32u);
    EXPECT_FALSE(rb.ensure_writable(61));
}

TEST(ByteRingBuffer, PeekFrontHandlesWrappedReadableData)
{
    ByteRingBuffer rb(8, 32);

    const std::array<std::byte, 6> first{std::byte{1}, std::byte{2}, std::byte{3},
                                         std::byte{4}, std::byte{5}, std::byte{6}};
    EXPECT_TRUE(rb.append(first));
    rb.consume(4);

    const std::array<std::byte, 4> second{std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
    EXPECT_TRUE(rb.append(second));

    std::array<std::byte, 6> out{};
    ASSERT_TRUE(rb.peek_front(out));
    EXPECT_EQ(out[0], std::byte{5});
    EXPECT_EQ(out[1], std::byte{6});
    EXPECT_EQ(out[2], std::byte{7});
    EXPECT_EQ(out[3], std::byte{8});
    EXPECT_EQ(out[4], std::byte{9});
    EXPECT_EQ(out[5], std::byte{10});
}

TEST(ByteRingBuffer, ShrinkToFitReturnsToBaselineAfterBurst)
{
    ByteRingBuffer rb(8, 64);
    EXPECT_TRUE(rb.ensure_writable(40));
    EXPECT_EQ(rb.capacity(), 64u);

    const std::array<std::byte, 40> payload{};
    EXPECT_TRUE(rb.append(payload));
    rb.consume(payload.size());

    rb.shrink_to_fit();
    EXPECT_EQ(rb.capacity(), 8u);
    EXPECT_EQ(rb.readable_size(), 0u);
    EXPECT_EQ(rb.writable_size(), 8u);
}

// ---------------------------------------------------------------------------
// SlotMap
// ---------------------------------------------------------------------------

TEST(SlotMap, InsertGetRemove)
{
    SlotMap<std::string> map;
    auto h = map.insert("hello");
    EXPECT_TRUE(h.is_valid());
    EXPECT_EQ(map.size(), 1u);

    auto* val = map.get(h);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, "hello");

    EXPECT_TRUE(map.remove(h));
    EXPECT_EQ(map.size(), 0u);
}

TEST(SlotMap, HandleInvalidationAfterRemove)
{
    SlotMap<int> map;
    auto h = map.insert(42);
    map.remove(h);

    // Old handle should be invalid due to generation bump
    EXPECT_FALSE(map.contains(h));
    EXPECT_EQ(map.get(h), nullptr);
}

TEST(SlotMap, DenseIterationNoGaps)
{
    SlotMap<int> map;
    auto h1 = map.insert(1);
    map.insert(2);
    map.insert(3);
    map.remove(h1);  // remove first element

    std::vector<int> values;
    for (auto& v : map)
    {
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

TEST(ObjectPool, CreateGetDestroyIsValid)
{
    ObjectPool<int> pool;
    auto h = pool.create(42);
    EXPECT_TRUE(pool.is_valid(h));

    auto* val = pool.get(h);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, 42);

    pool.destroy(h);
    EXPECT_FALSE(pool.is_valid(h));
    EXPECT_EQ(pool.get(h), nullptr);
}

// ---------------------------------------------------------------------------
// FlatMap
// ---------------------------------------------------------------------------

TEST(FlatMap, InsertAndFind)
{
    FlatMap<int, std::string> map;
    auto [it, inserted] = map.insert(1, "one");
    EXPECT_TRUE(inserted);
    EXPECT_EQ(it->second, "one");

    auto found = map.find(1);
    ASSERT_NE(found, map.end());
    EXPECT_EQ(found->second, "one");

    EXPECT_EQ(map.find(99), map.end());
}

TEST(FlatMap, OperatorBracket)
{
    FlatMap<std::string, int> map;
    map["x"] = 10;
    map["y"] = 20;
    EXPECT_EQ(map["x"], 10);
    EXPECT_EQ(map["y"], 20);
}

TEST(FlatMap, Erase)
{
    FlatMap<int, int> map;
    map.insert(1, 100);
    map.insert(2, 200);
    EXPECT_TRUE(map.erase(1));
    EXPECT_FALSE(map.contains(1));
    EXPECT_TRUE(map.contains(2));
    EXPECT_FALSE(map.erase(99));
}

TEST(FlatMap, MaintainsSortedOrder)
{
    FlatMap<int, int> map;
    map.insert(3, 30);
    map.insert(1, 10);
    map.insert(2, 20);

    std::vector<int> keys;
    for (auto& [k, v] : map)
    {
        keys.push_back(k);
    }
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST(FlatMap, ContainsEmptySize)
{
    FlatMap<int, int> map;
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0u);
    EXPECT_FALSE(map.contains(1));

    map.insert(1, 10);
    EXPECT_FALSE(map.empty());
    EXPECT_EQ(map.size(), 1u);
    EXPECT_TRUE(map.contains(1));
}

// ============================================================================
// Review issue: RingBuffer pop empty returns nullopt
// ============================================================================

TEST(RingBuffer, PopEmptyReturnsNullopt)
{
    RingBuffer<int> rb(4);
    auto val = rb.pop_front();
    EXPECT_FALSE(val.has_value());
}

TEST(RingBuffer, ClearResetsState)
{
    RingBuffer<int> rb(4);
    rb.push_back(1);
    rb.push_back(2);
    rb.clear();
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0u);

    // Should be able to push again after clear
    EXPECT_TRUE(rb.push_back(3));
    EXPECT_EQ(*rb.pop_front(), 3);
}

// ============================================================================
// Review issue: SlotMap generation bump on remove
// ============================================================================

TEST(SlotMap, SlotReuseWithNewGeneration)
{
    SlotMap<int> map;
    auto h1 = map.insert(100);
    map.remove(h1);

    // Insert again — reuses the same slot index but with bumped generation
    auto h2 = map.insert(200);
    EXPECT_TRUE(h2.is_valid());
    EXPECT_NE(h1.generation, h2.generation);  // generation should differ

    // Old handle should not work
    EXPECT_FALSE(map.contains(h1));
    EXPECT_EQ(map.get(h1), nullptr);

    // New handle should work
    EXPECT_TRUE(map.contains(h2));
    EXPECT_EQ(*map.get(h2), 200);
}

TEST(SlotMap, ClearInvalidatesAllHandles)
{
    SlotMap<int> map;
    auto h1 = map.insert(1);
    auto h2 = map.insert(2);
    auto h3 = map.insert(3);

    map.clear();
    EXPECT_EQ(map.size(), 0u);
    EXPECT_FALSE(map.contains(h1));
    EXPECT_FALSE(map.contains(h2));
    EXPECT_FALSE(map.contains(h3));

    // Can insert again after clear
    auto h4 = map.insert(4);
    EXPECT_TRUE(map.contains(h4));
    EXPECT_EQ(*map.get(h4), 4);
}

// ============================================================================
// Review issue: FlatMap duplicate insert
// ============================================================================

TEST(FlatMap, DuplicateInsertReturnsFalse)
{
    FlatMap<int, std::string> map;
    auto [it1, ok1] = map.insert(1, "first");
    EXPECT_TRUE(ok1);

    auto [it2, ok2] = map.insert(1, "second");
    EXPECT_FALSE(ok2);
    EXPECT_EQ(it2->second, "first");  // original value preserved
}

TEST(FlatMap, InsertOrAssignUpdatesExisting)
{
    FlatMap<int, std::string> map;
    map.insert_or_assign(1, "first");
    map.insert_or_assign(1, "updated");
    EXPECT_EQ(map.find(1)->second, "updated");
}

// ============================================================================
// Review issue: ObjectPool dense iteration after multiple create/destroy
// ============================================================================

TEST(ObjectPool, IterationAfterMixedCreateDestroy)
{
    ObjectPool<int> pool;
    [[maybe_unused]] auto h1 = pool.create(10);
    auto h2 = pool.create(20);
    auto h3 = pool.create(30);
    [[maybe_unused]] auto h4 = pool.create(40);

    pool.destroy(h2);
    pool.destroy(h3);

    std::vector<int> values;
    for (auto& v : pool)
    {
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

TEST(PagedSparseTable, AllocatesPagesLazily)
{
    PagedSparseTable<uint16_t, int> table;
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.allocated_page_count(), 0u);

    EXPECT_TRUE(table.insert(42, std::make_unique<int>(7)));
    EXPECT_EQ(table.size(), 1u);
    EXPECT_EQ(table.allocated_page_count(), 1u);
    ASSERT_NE(table.get(42), nullptr);
    EXPECT_EQ(*table.get(42), 7);
}

TEST(PagedSparseTable, SeparatePagesStaySparse)
{
    PagedSparseTable<uint16_t, int> table;
    EXPECT_TRUE(table.insert(0x0001, std::make_unique<int>(1)));
    EXPECT_TRUE(table.insert(0x1202, std::make_unique<int>(2)));
    EXPECT_TRUE(table.insert(0xFF03, std::make_unique<int>(3)));

    EXPECT_EQ(table.size(), 3u);
    EXPECT_EQ(table.allocated_page_count(), 3u);
    EXPECT_EQ(*table.get(0x0001), 1);
    EXPECT_EQ(*table.get(0x1202), 2);
    EXPECT_EQ(*table.get(0xFF03), 3);
    EXPECT_EQ(table.get(0x1203), nullptr);
}

TEST(PagedSparseTable, DuplicateInsertRejectedAndEraseUpdatesSize)
{
    PagedSparseTable<uint16_t, int> table;
    EXPECT_TRUE(table.insert(5000, std::make_unique<int>(11)));
    EXPECT_FALSE(table.insert(5000, std::make_unique<int>(22)));
    ASSERT_NE(table.get(5000), nullptr);
    EXPECT_EQ(*table.get(5000), 11);

    EXPECT_TRUE(table.erase(5000));
    EXPECT_FALSE(table.erase(5000));
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.get(5000), nullptr);
}

TEST(PagedSparseTable, ClearReleasesAllPages)
{
    PagedSparseTable<uint16_t, int> table;
    EXPECT_TRUE(table.insert(1, std::make_unique<int>(1)));
    EXPECT_TRUE(table.insert(0x2201, std::make_unique<int>(2)));
    EXPECT_EQ(table.allocated_page_count(), 2u);

    table.clear();
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.allocated_page_count(), 0u);
    EXPECT_EQ(table.get(1), nullptr);
    EXPECT_EQ(table.get(0x2201), nullptr);
}
