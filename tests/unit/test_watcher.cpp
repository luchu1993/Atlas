#include "server/watcher.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace atlas;

// ============================================================================
// Basic get/set
// ============================================================================

TEST(WatcherRegistry, AddAndGetInt)
{
    WatcherRegistry reg;
    int val = 42;
    reg.add("counter", val);

    auto result = reg.get("counter");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "42");
}

TEST(WatcherRegistry, AddAndGetDouble)
{
    WatcherRegistry reg;
    double d = 3.14;
    reg.add("ratio", d);

    auto result = reg.get("ratio");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

TEST(WatcherRegistry, AddAndGetBool)
{
    WatcherRegistry reg;
    bool flag = true;
    reg.add("enabled", flag);

    EXPECT_EQ(reg.get("enabled").value_or(""), "true");

    flag = false;
    EXPECT_EQ(reg.get("enabled").value_or(""), "false");
}

TEST(WatcherRegistry, GetMissingPathReturnsNullopt)
{
    WatcherRegistry reg;
    EXPECT_FALSE(reg.get("no/such/path").has_value());
}

// ============================================================================
// ReadOnly vs ReadWrite
// ============================================================================

TEST(WatcherRegistry, ReadOnlyBlocksSet)
{
    WatcherRegistry reg;
    int val = 10;
    reg.add("val", val);  // ReadOnly

    EXPECT_FALSE(reg.set("val", "99"));
    EXPECT_EQ(val, 10);
}

TEST(WatcherRegistry, ReadWriteAllowsSet)
{
    WatcherRegistry reg;
    int val = 10;
    reg.add_rw("val", val);

    EXPECT_TRUE(reg.set("val", "99"));
    EXPECT_EQ(val, 99);
}

TEST(WatcherRegistry, SetMissingPathReturnsFalse)
{
    WatcherRegistry reg;
    EXPECT_FALSE(reg.set("no/such/path", "1"));
}

TEST(WatcherRegistry, SetInvalidValueReturnsFalse)
{
    WatcherRegistry reg;
    int val = 5;
    reg.add_rw("val", val);

    EXPECT_FALSE(reg.set("val", "not_a_number"));
    EXPECT_EQ(val, 5);
}

// ============================================================================
// Hierarchical paths
// ============================================================================

TEST(WatcherRegistry, HierarchicalPaths)
{
    WatcherRegistry reg;
    int sent = 100;
    int recv = 200;
    reg.add("network/bytes_sent", sent);
    reg.add("network/bytes_recv", recv);

    EXPECT_EQ(reg.get("network/bytes_sent").value_or(""), "100");
    EXPECT_EQ(reg.get("network/bytes_recv").value_or(""), "200");
    EXPECT_EQ(reg.size(), 2u);
}

TEST(WatcherRegistry, DeepHierarchicalPath)
{
    WatcherRegistry reg;
    double ms = 1.5;
    reg.add("tick/stats/last_ms", ms);

    EXPECT_TRUE(reg.get("tick/stats/last_ms").has_value());
    EXPECT_FALSE(reg.get("tick/stats").has_value());  // directory node, not a leaf
}

// ============================================================================
// Getter lambda (FunctionWatcher)
// ============================================================================

TEST(WatcherRegistry, GetterLambda)
{
    WatcherRegistry reg;
    int counter = 0;
    reg.add<int>("counter", [&counter]() { return counter; });

    counter = 7;
    EXPECT_EQ(reg.get("counter").value_or(""), "7");

    counter = 42;
    EXPECT_EQ(reg.get("counter").value_or(""), "42");
}

TEST(WatcherRegistry, GetterSetterLambda)
{
    WatcherRegistry reg;
    int val = 0;
    reg.add_rw<int>(
        "val", [&val]() { return val; },
        [&val](int v)
        {
            val = v;
            return true;
        });

    EXPECT_TRUE(reg.set("val", "55"));
    EXPECT_EQ(val, 55);
    EXPECT_EQ(reg.get("val").value_or(""), "55");
}

// ============================================================================
// list()
// ============================================================================

TEST(WatcherRegistry, ListAll)
{
    WatcherRegistry reg;
    int a = 0, b = 0, c = 0;
    reg.add("app/name", a);
    reg.add("app/pid", b);
    reg.add("tick/count", c);

    auto paths = reg.list();
    ASSERT_EQ(paths.size(), 3u);
    // map iteration is sorted
    EXPECT_EQ(paths[0], "app/name");
    EXPECT_EQ(paths[1], "app/pid");
    EXPECT_EQ(paths[2], "tick/count");
}

TEST(WatcherRegistry, ListWithPrefix)
{
    WatcherRegistry reg;
    int a = 0, b = 0, c = 0;
    reg.add("app/name", a);
    reg.add("app/pid", b);
    reg.add("tick/count", c);

    auto paths = reg.list("app");
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], "app/name");
    EXPECT_EQ(paths[1], "app/pid");
}

TEST(WatcherRegistry, ListMissingPrefixReturnsEmpty)
{
    WatcherRegistry reg;
    int a = 0;
    reg.add("app/name", a);

    EXPECT_TRUE(reg.list("network").empty());
}

// ============================================================================
// snapshot()
// ============================================================================

TEST(WatcherRegistry, Snapshot)
{
    WatcherRegistry reg;
    int x = 1;
    bool y = true;
    reg.add("x", x);
    reg.add("y", y);

    auto snap = reg.snapshot();
    ASSERT_EQ(snap.size(), 2u);
    // sorted by path (map order)
    EXPECT_EQ(snap[0].first, "x");
    EXPECT_EQ(snap[0].second, "1");
    EXPECT_EQ(snap[1].first, "y");
    EXPECT_EQ(snap[1].second, "true");
}

TEST(WatcherRegistry, SnapshotReflectsCurrentValues)
{
    WatcherRegistry reg;
    int counter = 0;
    reg.add<int>("counter", [&counter]() { return counter; });

    counter = 10;
    auto snap1 = reg.snapshot();
    EXPECT_EQ(snap1[0].second, "10");

    counter = 20;
    auto snap2 = reg.snapshot();
    EXPECT_EQ(snap2[0].second, "20");
}

// ============================================================================
// Overwrite existing path
// ============================================================================

TEST(WatcherRegistry, OverwriteExistingPath)
{
    WatcherRegistry reg;
    int old_val = 1;
    int new_val = 99;

    reg.add("key", old_val);
    EXPECT_EQ(reg.size(), 1u);

    reg.add("key", new_val);
    EXPECT_EQ(reg.size(), 1u);  // count must not increase

    EXPECT_EQ(reg.get("key").value_or(""), "99");
}

// ============================================================================
// Size tracking
// ============================================================================

TEST(WatcherRegistry, SizeTracking)
{
    WatcherRegistry reg;
    EXPECT_EQ(reg.size(), 0u);

    int a = 0, b = 0;
    reg.add("a", a);
    EXPECT_EQ(reg.size(), 1u);

    reg.add("b", b);
    EXPECT_EQ(reg.size(), 2u);
}

// ============================================================================
// Bool ReadWrite
// ============================================================================

TEST(WatcherRegistry, BoolReadWrite)
{
    WatcherRegistry reg;
    bool flag = false;
    reg.add_rw("flag", flag);

    EXPECT_TRUE(reg.set("flag", "true"));
    EXPECT_TRUE(flag);

    EXPECT_TRUE(reg.set("flag", "0"));
    EXPECT_FALSE(flag);

    EXPECT_FALSE(reg.set("flag", "maybe"));
}

// ============================================================================
// String watcher
// ============================================================================

TEST(WatcherRegistry, StringWatcher)
{
    WatcherRegistry reg;
    std::string name = "atlas";
    reg.add("app/name", name);

    EXPECT_EQ(reg.get("app/name").value_or(""), "atlas");
}
