#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "machined/lease_store.h"
#include "network/address.h"

namespace atlas::machined {
namespace {

auto MakeAddr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

TEST(LeaseStore, FirstAcquireSucceeds) {
  LeaseStore store;
  const auto now = Clock::now();
  auto outcome = store.Acquire("reviver/cellappmgr", "host-a:reviver", 1000, MakeAddr(27001), now);
  EXPECT_EQ(outcome.result, LeaseStore::AcquireResult::kAcquired);
  EXPECT_EQ(store.size(), 1u);
  auto entry = store.Find("reviver/cellappmgr");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->holder_id, "host-a:reviver");
}

TEST(LeaseStore, SameHolderRenewsBumpsExpiry) {
  LeaseStore store;
  const auto t0 = Clock::now();
  (void)store.Acquire("k", "holder", 1000, MakeAddr(1), t0);
  const auto t1 = t0 + std::chrono::milliseconds(500);
  auto outcome = store.Acquire("k", "holder", 1000, MakeAddr(1), t1);
  EXPECT_EQ(outcome.result, LeaseStore::AcquireResult::kRenewed);
  auto entry = store.Find("k");
  ASSERT_TRUE(entry.has_value());
  // expiry should have advanced by ~500ms (t1 + 1000 vs t0 + 1000).
  EXPECT_GT(entry->expires_at, t0 + std::chrono::milliseconds(1000));
}

TEST(LeaseStore, OtherHolderRejectedWhileHeld) {
  LeaseStore store;
  const auto t0 = Clock::now();
  (void)store.Acquire("k", "holder-a", 1000, MakeAddr(1), t0);
  const auto t1 = t0 + std::chrono::milliseconds(200);
  auto outcome = store.Acquire("k", "holder-b", 1000, MakeAddr(2), t1);
  EXPECT_EQ(outcome.result, LeaseStore::AcquireResult::kRejected);
  EXPECT_EQ(outcome.current_holder, "holder-a");
  EXPECT_GT(outcome.current_expires_in_ms, 700);  // ~800ms left
  EXPECT_LE(outcome.current_expires_in_ms, 1000);
}

TEST(LeaseStore, OtherHolderAcquiresAfterExpiry) {
  LeaseStore store;
  const auto t0 = Clock::now();
  (void)store.Acquire("k", "holder-a", 1000, MakeAddr(1), t0);
  const auto t_after = t0 + std::chrono::milliseconds(1500);
  auto outcome = store.Acquire("k", "holder-b", 1000, MakeAddr(2), t_after);
  EXPECT_EQ(outcome.result, LeaseStore::AcquireResult::kAcquired);
  auto entry = store.Find("k");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->holder_id, "holder-b");
}

TEST(LeaseStore, ReleaseDropsEntry) {
  LeaseStore store;
  (void)store.Acquire("k", "holder", 1000, MakeAddr(1), Clock::now());
  EXPECT_TRUE(store.Release("k", "holder"));
  EXPECT_EQ(store.size(), 0u);
}

TEST(LeaseStore, ReleaseRejectsWrongHolder) {
  LeaseStore store;
  (void)store.Acquire("k", "holder-a", 1000, MakeAddr(1), Clock::now());
  EXPECT_FALSE(store.Release("k", "holder-b"));
  EXPECT_EQ(store.size(), 1u);
}

TEST(LeaseStore, ReleaseOnUnknownKeyIsFalse) {
  LeaseStore store;
  EXPECT_FALSE(store.Release("missing", "holder"));
}

TEST(LeaseStore, PruneExpiredRemovesOnlyExpired) {
  LeaseStore store;
  const auto t0 = Clock::now();
  (void)store.Acquire("short", "h1", 100, MakeAddr(1), t0);
  (void)store.Acquire("long", "h2", 60000, MakeAddr(2), t0);
  const auto t_after = t0 + std::chrono::milliseconds(500);
  EXPECT_EQ(store.PruneExpired(t_after), 1u);
  EXPECT_FALSE(store.Find("short").has_value());
  EXPECT_TRUE(store.Find("long").has_value());
}

TEST(LeaseStore, DropByHolderAddressClearsMatchingEntries) {
  LeaseStore store;
  const auto t0 = Clock::now();
  (void)store.Acquire("k1", "h1", 60000, MakeAddr(1), t0);
  (void)store.Acquire("k2", "h2", 60000, MakeAddr(2), t0);
  (void)store.Acquire("k3", "h3", 60000, MakeAddr(1), t0);
  EXPECT_EQ(store.DropByHolderAddress(MakeAddr(1)), 2u);
  EXPECT_FALSE(store.Find("k1").has_value());
  EXPECT_TRUE(store.Find("k2").has_value());
  EXPECT_FALSE(store.Find("k3").has_value());
}

}  // namespace
}  // namespace atlas::machined
