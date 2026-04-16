#include <gtest/gtest.h>

#include "machined/listener_manager.h"

using namespace atlas;
using namespace atlas::machined;

// ============================================================================
// Helpers — use fake channel pointers (never dereferenced in these tests)
// ============================================================================

namespace {

// NOLINTNEXTLINE(performance-no-int-to-ptr)
Channel* fake_ch(uintptr_t v) {
  return reinterpret_cast<Channel*>(v);
}

}  // namespace

TEST(ListenerManager, AddAndCount) {
  ListenerManager mgr;
  EXPECT_EQ(mgr.SubscriptionCount(), 0u);

  mgr.AddListener(fake_ch(1), ListenerType::kBirth, ProcessType::kBaseApp);
  EXPECT_EQ(mgr.SubscriptionCount(), 1u);

  mgr.AddListener(fake_ch(2), ListenerType::kDeath, ProcessType::kCellApp);
  EXPECT_EQ(mgr.SubscriptionCount(), 2u);
}

TEST(ListenerManager, DuplicateReplaces) {
  ListenerManager mgr;
  mgr.AddListener(fake_ch(1), ListenerType::kBirth, ProcessType::kBaseApp);
  EXPECT_EQ(mgr.SubscriptionCount(), 1u);

  // Same channel + target_type → replaces
  mgr.AddListener(fake_ch(1), ListenerType::kBoth, ProcessType::kBaseApp);
  EXPECT_EQ(mgr.SubscriptionCount(), 1u);
}

TEST(ListenerManager, DifferentTargetTypeAdded) {
  ListenerManager mgr;
  mgr.AddListener(fake_ch(1), ListenerType::kBirth, ProcessType::kBaseApp);
  mgr.AddListener(fake_ch(1), ListenerType::kBirth, ProcessType::kCellApp);
  EXPECT_EQ(mgr.SubscriptionCount(), 2u);
}

TEST(ListenerManager, RemoveAll) {
  ListenerManager mgr;
  mgr.AddListener(fake_ch(1), ListenerType::kBoth, ProcessType::kBaseApp);
  mgr.AddListener(fake_ch(1), ListenerType::kBoth, ProcessType::kCellApp);
  mgr.AddListener(fake_ch(2), ListenerType::kBoth, ProcessType::kBaseApp);

  mgr.RemoveAll(fake_ch(1));
  EXPECT_EQ(mgr.SubscriptionCount(), 1u);  // only ch(2) remains

  mgr.RemoveAll(fake_ch(2));
  EXPECT_EQ(mgr.SubscriptionCount(), 0u);
}

TEST(ListenerManager, RemoveNonexistent) {
  ListenerManager mgr;
  mgr.AddListener(fake_ch(1), ListenerType::kBoth, ProcessType::kBaseApp);

  // Removing a channel that was not subscribed is a no-op
  mgr.RemoveAll(fake_ch(99));
  EXPECT_EQ(mgr.SubscriptionCount(), 1u);
}

// notify_birth/death require connected channels; since we cannot create real
// Channel objects in a unit test without a full network stack, we only verify
// that the manager does not crash when all channels are disconnected (nullptr).
TEST(ListenerManager, NotifyBirthSkipsNullChannel) {
  ListenerManager mgr;
  // Deliberately subscribe a null channel (simulates a channel that was
  // never connected)
  mgr.AddListener(nullptr, ListenerType::kBoth, ProcessType::kBaseApp);

  BirthNotification notif;
  notif.process_type = ProcessType::kBaseApp;
  notif.name = "baseapp-1";

  // Should not crash
  EXPECT_NO_THROW(mgr.NotifyBirth(notif));
}

TEST(ListenerManager, NotifyDeathSkipsNullChannel) {
  ListenerManager mgr;
  mgr.AddListener(nullptr, ListenerType::kBoth, ProcessType::kDbApp);

  DeathNotification notif;
  notif.process_type = ProcessType::kDbApp;
  notif.name = "dbapp-1";

  EXPECT_NO_THROW(mgr.NotifyDeath(notif));
}
