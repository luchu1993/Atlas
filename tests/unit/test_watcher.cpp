#include <string>

#include <gtest/gtest.h>

#include "server/watcher.h"

using namespace atlas;

// ============================================================================
// Basic get/set
// ============================================================================

TEST(WatcherRegistry, AddAndGetInt) {
  WatcherRegistry reg;
  int val = 42;
  reg.Add("counter", val);

  auto result = reg.Get("counter");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "42");
}

TEST(WatcherRegistry, AddAndGetDouble) {
  WatcherRegistry reg;
  double d = 3.14;
  reg.Add("ratio", d);

  auto result = reg.Get("ratio");
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->empty());
}

TEST(WatcherRegistry, AddAndGetBool) {
  WatcherRegistry reg;
  bool flag = true;
  reg.Add("enabled", flag);

  EXPECT_EQ(reg.Get("enabled").value_or(""), "true");

  flag = false;
  EXPECT_EQ(reg.Get("enabled").value_or(""), "false");
}

TEST(WatcherRegistry, GetMissingPathReturnsNullopt) {
  WatcherRegistry reg;
  EXPECT_FALSE(reg.Get("no/such/path").has_value());
}

// ============================================================================
// ReadOnly vs ReadWrite
// ============================================================================

TEST(WatcherRegistry, ReadOnlyBlocksSet) {
  WatcherRegistry reg;
  int val = 10;
  reg.Add("val", val);  // ReadOnly

  EXPECT_FALSE(reg.Set("val", "99"));
  EXPECT_EQ(val, 10);
}

TEST(WatcherRegistry, ReadWriteAllowsSet) {
  WatcherRegistry reg;
  int val = 10;
  reg.AddRw("val", val);

  EXPECT_TRUE(reg.Set("val", "99"));
  EXPECT_EQ(val, 99);
}

TEST(WatcherRegistry, SetMissingPathReturnsFalse) {
  WatcherRegistry reg;
  EXPECT_FALSE(reg.Set("no/such/path", "1"));
}

TEST(WatcherRegistry, SetInvalidValueReturnsFalse) {
  WatcherRegistry reg;
  int val = 5;
  reg.AddRw("val", val);

  EXPECT_FALSE(reg.Set("val", "not_a_number"));
  EXPECT_EQ(val, 5);
}

// ============================================================================
// Hierarchical paths
// ============================================================================

TEST(WatcherRegistry, HierarchicalPaths) {
  WatcherRegistry reg;
  int sent = 100;
  int recv = 200;
  reg.Add("network/bytes_sent", sent);
  reg.Add("network/bytes_recv", recv);

  EXPECT_EQ(reg.Get("network/bytes_sent").value_or(""), "100");
  EXPECT_EQ(reg.Get("network/bytes_recv").value_or(""), "200");
  EXPECT_EQ(reg.size(), 2u);
}

TEST(WatcherRegistry, DeepHierarchicalPath) {
  WatcherRegistry reg;
  double ms = 1.5;
  reg.Add("tick/stats/last_ms", ms);

  EXPECT_TRUE(reg.Get("tick/stats/last_ms").has_value());
  EXPECT_FALSE(reg.Get("tick/stats").has_value());  // directory node, not a leaf
}

// ============================================================================
// Getter lambda (FunctionWatcher)
// ============================================================================

TEST(WatcherRegistry, GetterLambda) {
  WatcherRegistry reg;
  int counter = 0;
  reg.Add<int>("counter", [&counter]() { return counter; });

  counter = 7;
  EXPECT_EQ(reg.Get("counter").value_or(""), "7");

  counter = 42;
  EXPECT_EQ(reg.Get("counter").value_or(""), "42");
}

TEST(WatcherRegistry, GetterSetterLambda) {
  WatcherRegistry reg;
  int val = 0;
  reg.AddRw<int>(
      "val", [&val]() { return val; },
      [&val](int v) {
        val = v;
        return true;
      });

  EXPECT_TRUE(reg.Set("val", "55"));
  EXPECT_EQ(val, 55);
  EXPECT_EQ(reg.Get("val").value_or(""), "55");
}

// ============================================================================
// list()
// ============================================================================

TEST(WatcherRegistry, ListAll) {
  WatcherRegistry reg;
  int a = 0, b = 0, c = 0;
  reg.Add("app/name", a);
  reg.Add("app/pid", b);
  reg.Add("tick/count", c);

  auto paths = reg.List();
  ASSERT_EQ(paths.size(), 3u);
  // map iteration is sorted
  EXPECT_EQ(paths[0], "app/name");
  EXPECT_EQ(paths[1], "app/pid");
  EXPECT_EQ(paths[2], "tick/count");
}

TEST(WatcherRegistry, ListWithPrefix) {
  WatcherRegistry reg;
  int a = 0, b = 0, c = 0;
  reg.Add("app/name", a);
  reg.Add("app/pid", b);
  reg.Add("tick/count", c);

  auto paths = reg.List("app");
  ASSERT_EQ(paths.size(), 2u);
  EXPECT_EQ(paths[0], "app/name");
  EXPECT_EQ(paths[1], "app/pid");
}

TEST(WatcherRegistry, ListMissingPrefixReturnsEmpty) {
  WatcherRegistry reg;
  int a = 0;
  reg.Add("app/name", a);

  EXPECT_TRUE(reg.List("network").empty());
}

// ============================================================================
// snapshot()
// ============================================================================

TEST(WatcherRegistry, Snapshot) {
  WatcherRegistry reg;
  int x = 1;
  bool y = true;
  reg.Add("x", x);
  reg.Add("y", y);

  auto snap = reg.Snapshot();
  ASSERT_EQ(snap.size(), 2u);
  // sorted by path (map order)
  EXPECT_EQ(snap[0].first, "x");
  EXPECT_EQ(snap[0].second, "1");
  EXPECT_EQ(snap[1].first, "y");
  EXPECT_EQ(snap[1].second, "true");
}

TEST(WatcherRegistry, SnapshotReflectsCurrentValues) {
  WatcherRegistry reg;
  int counter = 0;
  reg.Add<int>("counter", [&counter]() { return counter; });

  counter = 10;
  auto snap1 = reg.Snapshot();
  EXPECT_EQ(snap1[0].second, "10");

  counter = 20;
  auto snap2 = reg.Snapshot();
  EXPECT_EQ(snap2[0].second, "20");
}

// ============================================================================
// Overwrite existing path
// ============================================================================

TEST(WatcherRegistry, OverwriteExistingPath) {
  WatcherRegistry reg;
  int old_val = 1;
  int new_val = 99;

  reg.Add("key", old_val);
  EXPECT_EQ(reg.size(), 1u);

  reg.Add("key", new_val);
  EXPECT_EQ(reg.size(), 1u);  // count must not increase

  EXPECT_EQ(reg.Get("key").value_or(""), "99");
}

// ============================================================================
// Size tracking
// ============================================================================

TEST(WatcherRegistry, SizeTracking) {
  WatcherRegistry reg;
  EXPECT_EQ(reg.size(), 0u);

  int a = 0, b = 0;
  reg.Add("a", a);
  EXPECT_EQ(reg.size(), 1u);

  reg.Add("b", b);
  EXPECT_EQ(reg.size(), 2u);
}

// ============================================================================
// Bool ReadWrite
// ============================================================================

TEST(WatcherRegistry, BoolReadWrite) {
  WatcherRegistry reg;
  bool flag = false;
  reg.AddRw("flag", flag);

  EXPECT_TRUE(reg.Set("flag", "true"));
  EXPECT_TRUE(flag);

  EXPECT_TRUE(reg.Set("flag", "0"));
  EXPECT_FALSE(flag);

  EXPECT_FALSE(reg.Set("flag", "maybe"));
}

// ============================================================================
// String watcher
// ============================================================================

TEST(WatcherRegistry, StringWatcher) {
  WatcherRegistry reg;
  std::string name = "atlas";
  reg.Add("app/name", name);

  EXPECT_EQ(reg.Get("app/name").value_or(""), "atlas");
}
