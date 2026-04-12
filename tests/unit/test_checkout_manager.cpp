#include "checkout_manager.hpp"

#include <gtest/gtest.h>

using namespace atlas;

namespace
{

CheckoutInfo make_owner(uint32_t ip, uint16_t port, uint32_t app_id, uint32_t entity_id)
{
    CheckoutInfo info;
    info.base_addr = Address(ip, port);
    info.app_id = app_id;
    info.entity_id = entity_id;
    return info;
}

}  // namespace

// ============================================================================
// Basic checkout / checkin
// ============================================================================

TEST(CheckoutManager, InitiallyEmpty)
{
    CheckoutManager mgr;
    EXPECT_EQ(mgr.size(), 0u);
    EXPECT_FALSE(mgr.get_owner(1, 1).has_value());
}

TEST(CheckoutManager, TryCheckoutSucceeds)
{
    CheckoutManager mgr;
    auto owner = make_owner(0x7F000001, 7100, 1, 100);

    auto result = mgr.try_checkout(42, 1, owner);
    EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::Success);
    EXPECT_EQ(mgr.size(), 1u);
}

TEST(CheckoutManager, ConfirmCheckoutMakesOwnerVisible)
{
    CheckoutManager mgr;
    auto owner = make_owner(0x7F000001, 7100, 1, 100);

    (void)mgr.try_checkout(42, 1, owner);
    mgr.confirm_checkout(42, 1);

    auto got = mgr.get_owner(42, 1);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->app_id, 1u);
    EXPECT_EQ(got->entity_id, 100u);
}

TEST(CheckoutManager, CheckinRemovesEntry)
{
    CheckoutManager mgr;
    auto owner = make_owner(0x7F000001, 7100, 1, 100);

    (void)mgr.try_checkout(10, 2, owner);
    mgr.confirm_checkout(10, 2);
    mgr.checkin(10, 2);

    EXPECT_EQ(mgr.size(), 0u);
    EXPECT_FALSE(mgr.get_owner(10, 2).has_value());
}

// ============================================================================
// Concurrent / duplicate checkout
// ============================================================================

TEST(CheckoutManager, DuplicateCheckoutReturnsAlreadyCheckedOut)
{
    CheckoutManager mgr;
    auto owner1 = make_owner(0x7F000001, 7100, 1, 100);
    auto owner2 = make_owner(0x7F000002, 7100, 2, 200);

    (void)mgr.try_checkout(42, 1, owner1);
    mgr.confirm_checkout(42, 1);

    auto result = mgr.try_checkout(42, 1, owner2);
    EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::AlreadyCheckedOut);
    EXPECT_EQ(result.current_owner.app_id, 1u);
}

TEST(CheckoutManager, PendingCheckoutBlocksConcurrentRequest)
{
    CheckoutManager mgr;
    auto owner1 = make_owner(0x7F000001, 7100, 1, 100);
    auto owner2 = make_owner(0x7F000002, 7100, 2, 200);

    // First call: Checking state (DB in-flight)
    (void)mgr.try_checkout(42, 1, owner1);
    // Not confirmed yet!

    auto result = mgr.try_checkout(42, 1, owner2);
    EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::PendingCheckout);
    EXPECT_EQ(result.current_owner.base_addr, owner1.base_addr);
    EXPECT_EQ(result.current_owner.app_id, owner1.app_id);
    EXPECT_EQ(result.current_owner.entity_id, owner1.entity_id);
}

// ============================================================================
// Release checkout (DB failure rollback)
// ============================================================================

TEST(CheckoutManager, ReleaseCheckoutAllowsRetry)
{
    CheckoutManager mgr;
    auto owner = make_owner(0x7F000001, 7100, 1, 100);

    (void)mgr.try_checkout(42, 1, owner);
    mgr.release_checkout(42, 1);  // DB operation failed

    EXPECT_EQ(mgr.size(), 0u);

    // Retry should succeed
    auto result = mgr.try_checkout(42, 1, owner);
    EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::Success);
}

// ============================================================================
// Different keys are independent
// ============================================================================

TEST(CheckoutManager, DifferentDbidAreIndependent)
{
    CheckoutManager mgr;
    auto owner = make_owner(0x7F000001, 7100, 1, 100);

    (void)mgr.try_checkout(1, 1, owner);
    (void)mgr.try_checkout(2, 1, owner);
    (void)mgr.try_checkout(1, 2, owner);  // different type_id

    EXPECT_EQ(mgr.size(), 3u);
}

// ============================================================================
// clear_all_for (BaseApp death)
// ============================================================================

TEST(CheckoutManager, ClearAllForRemovesCorrectEntries)
{
    CheckoutManager mgr;
    Address dead_app(0x7F000001, 7100);
    Address alive_app(0x7F000002, 7100);

    auto dead_owner = make_owner(0x7F000001, 7100, 1, 0);
    auto alive_owner = make_owner(0x7F000002, 7100, 2, 0);

    (void)mgr.try_checkout(10, 1, dead_owner);
    mgr.confirm_checkout(10, 1);
    (void)mgr.try_checkout(11, 1, dead_owner);
    mgr.confirm_checkout(11, 1);
    (void)mgr.try_checkout(20, 1, alive_owner);
    mgr.confirm_checkout(20, 1);

    EXPECT_EQ(mgr.size(), 3u);

    int cleared = mgr.clear_all_for(dead_app);
    EXPECT_EQ(cleared, 2);
    EXPECT_EQ(mgr.size(), 1u);
    EXPECT_TRUE(mgr.get_owner(20, 1).has_value());
    EXPECT_FALSE(mgr.get_owner(10, 1).has_value());
    EXPECT_FALSE(mgr.get_owner(11, 1).has_value());
}

TEST(CheckoutManager, ClearAllForNoMatchReturnsZero)
{
    CheckoutManager mgr;
    auto owner = make_owner(0x7F000001, 7100, 1, 100);
    (void)mgr.try_checkout(1, 1, owner);
    mgr.confirm_checkout(1, 1);

    Address other(0x7F000099, 7100);
    int cleared = mgr.clear_all_for(other);
    EXPECT_EQ(cleared, 0);
    EXPECT_EQ(mgr.size(), 1u);
}
