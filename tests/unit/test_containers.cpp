#include <gtest/gtest.h>
#include "foundation/containers/ring_buffer.hpp"
#include "foundation/containers/slot_map.hpp"
#include "foundation/containers/object_pool.hpp"
#include "foundation/containers/flat_map.hpp"

#include <algorithm>
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
