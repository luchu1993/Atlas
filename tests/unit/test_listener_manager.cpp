#include "machined/listener_manager.hpp"

#include <gtest/gtest.h>

using namespace atlas;
using namespace atlas::machined;

// ============================================================================
// Helpers — use fake channel pointers (never dereferenced in these tests)
// ============================================================================

namespace
{

// NOLINTNEXTLINE(performance-no-int-to-ptr)
Channel* fake_ch(uintptr_t v)
{
    return reinterpret_cast<Channel*>(v);
}

}  // namespace

TEST(ListenerManager, AddAndCount)
{
    ListenerManager mgr;
    EXPECT_EQ(mgr.subscription_count(), 0u);

    mgr.add_listener(fake_ch(1), ListenerType::Birth, ProcessType::BaseApp);
    EXPECT_EQ(mgr.subscription_count(), 1u);

    mgr.add_listener(fake_ch(2), ListenerType::Death, ProcessType::CellApp);
    EXPECT_EQ(mgr.subscription_count(), 2u);
}

TEST(ListenerManager, DuplicateReplaces)
{
    ListenerManager mgr;
    mgr.add_listener(fake_ch(1), ListenerType::Birth, ProcessType::BaseApp);
    EXPECT_EQ(mgr.subscription_count(), 1u);

    // Same channel + target_type → replaces
    mgr.add_listener(fake_ch(1), ListenerType::Both, ProcessType::BaseApp);
    EXPECT_EQ(mgr.subscription_count(), 1u);
}

TEST(ListenerManager, DifferentTargetTypeAdded)
{
    ListenerManager mgr;
    mgr.add_listener(fake_ch(1), ListenerType::Birth, ProcessType::BaseApp);
    mgr.add_listener(fake_ch(1), ListenerType::Birth, ProcessType::CellApp);
    EXPECT_EQ(mgr.subscription_count(), 2u);
}

TEST(ListenerManager, RemoveAll)
{
    ListenerManager mgr;
    mgr.add_listener(fake_ch(1), ListenerType::Both, ProcessType::BaseApp);
    mgr.add_listener(fake_ch(1), ListenerType::Both, ProcessType::CellApp);
    mgr.add_listener(fake_ch(2), ListenerType::Both, ProcessType::BaseApp);

    mgr.remove_all(fake_ch(1));
    EXPECT_EQ(mgr.subscription_count(), 1u);  // only ch(2) remains

    mgr.remove_all(fake_ch(2));
    EXPECT_EQ(mgr.subscription_count(), 0u);
}

TEST(ListenerManager, RemoveNonexistent)
{
    ListenerManager mgr;
    mgr.add_listener(fake_ch(1), ListenerType::Both, ProcessType::BaseApp);

    // Removing a channel that was not subscribed is a no-op
    mgr.remove_all(fake_ch(99));
    EXPECT_EQ(mgr.subscription_count(), 1u);
}

// notify_birth/death require connected channels; since we cannot create real
// Channel objects in a unit test without a full network stack, we only verify
// that the manager does not crash when all channels are disconnected (nullptr).
TEST(ListenerManager, NotifyBirthSkipsNullChannel)
{
    ListenerManager mgr;
    // Deliberately subscribe a null channel (simulates a channel that was
    // never connected)
    mgr.add_listener(nullptr, ListenerType::Both, ProcessType::BaseApp);

    BirthNotification notif;
    notif.process_type = ProcessType::BaseApp;
    notif.name = "baseapp-1";

    // Should not crash
    EXPECT_NO_THROW(mgr.notify_birth(notif));
}

TEST(ListenerManager, NotifyDeathSkipsNullChannel)
{
    ListenerManager mgr;
    mgr.add_listener(nullptr, ListenerType::Both, ProcessType::DBApp);

    DeathNotification notif;
    notif.process_type = ProcessType::DBApp;
    notif.name = "dbapp-1";

    EXPECT_NO_THROW(mgr.notify_death(notif));
}
