#include <gtest/gtest.h>

#include "checkout_manager.h"

using namespace atlas;

namespace {

CheckoutInfo make_owner(uint32_t ip, uint16_t port, uint32_t app_id, uint32_t entity_id) {
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

TEST(CheckoutManager, InitiallyEmpty) {
  CheckoutManager mgr;
  EXPECT_EQ(mgr.size(), 0u);
  EXPECT_FALSE(mgr.GetOwner(1, 1).has_value());
}

TEST(CheckoutManager, TryCheckoutSucceeds) {
  CheckoutManager mgr;
  auto owner = make_owner(0x7F000001, 7100, 1, 100);

  auto result = mgr.TryCheckout(42, 1, owner);
  EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::kSuccess);
  EXPECT_EQ(mgr.size(), 1u);
}

TEST(CheckoutManager, ConfirmCheckoutMakesOwnerVisible) {
  CheckoutManager mgr;
  auto owner = make_owner(0x7F000001, 7100, 1, 100);

  (void)mgr.TryCheckout(42, 1, owner);
  mgr.ConfirmCheckout(42, 1);

  auto got = mgr.GetOwner(42, 1);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->app_id, 1u);
  EXPECT_EQ(got->entity_id, 100u);
}

TEST(CheckoutManager, CheckinRemovesEntry) {
  CheckoutManager mgr;
  auto owner = make_owner(0x7F000001, 7100, 1, 100);

  (void)mgr.TryCheckout(10, 2, owner);
  mgr.ConfirmCheckout(10, 2);
  mgr.Checkin(10, 2);

  EXPECT_EQ(mgr.size(), 0u);
  EXPECT_FALSE(mgr.GetOwner(10, 2).has_value());
}

// ============================================================================
// Concurrent / duplicate checkout
// ============================================================================

TEST(CheckoutManager, DuplicateCheckoutReturnsAlreadyCheckedOut) {
  CheckoutManager mgr;
  auto owner1 = make_owner(0x7F000001, 7100, 1, 100);
  auto owner2 = make_owner(0x7F000002, 7100, 2, 200);

  (void)mgr.TryCheckout(42, 1, owner1);
  mgr.ConfirmCheckout(42, 1);

  auto result = mgr.TryCheckout(42, 1, owner2);
  EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::kAlreadyCheckedOut);
  EXPECT_EQ(result.current_owner.app_id, 1u);
}

TEST(CheckoutManager, PendingCheckoutBlocksConcurrentRequest) {
  CheckoutManager mgr;
  auto owner1 = make_owner(0x7F000001, 7100, 1, 100);
  auto owner2 = make_owner(0x7F000002, 7100, 2, 200);

  // First call: Checking state (DB in-flight)
  (void)mgr.TryCheckout(42, 1, owner1);
  // Not confirmed yet!

  auto result = mgr.TryCheckout(42, 1, owner2);
  EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::kPendingCheckout);
  EXPECT_EQ(result.current_owner.base_addr, owner1.base_addr);
  EXPECT_EQ(result.current_owner.app_id, owner1.app_id);
  EXPECT_EQ(result.current_owner.entity_id, owner1.entity_id);
}

// ============================================================================
// Release checkout (DB failure rollback)
// ============================================================================

TEST(CheckoutManager, ReleaseCheckoutAllowsRetry) {
  CheckoutManager mgr;
  auto owner = make_owner(0x7F000001, 7100, 1, 100);

  (void)mgr.TryCheckout(42, 1, owner);
  mgr.ReleaseCheckout(42, 1);  // DB operation failed

  EXPECT_EQ(mgr.size(), 0u);

  // Retry should succeed
  auto result = mgr.TryCheckout(42, 1, owner);
  EXPECT_EQ(result.status, CheckoutManager::CheckoutStatus::kSuccess);
}

// ============================================================================
// Different keys are independent
// ============================================================================

TEST(CheckoutManager, DifferentDbidAreIndependent) {
  CheckoutManager mgr;
  auto owner = make_owner(0x7F000001, 7100, 1, 100);

  (void)mgr.TryCheckout(1, 1, owner);
  (void)mgr.TryCheckout(2, 1, owner);
  (void)mgr.TryCheckout(1, 2, owner);  // different type_id

  EXPECT_EQ(mgr.size(), 3u);
}

// ============================================================================
// ClearAllFor (BaseApp death)
// ============================================================================

TEST(CheckoutManager, ClearAllForRemovesCorrectEntries) {
  CheckoutManager mgr;
  Address dead_app(0x7F000001, 7100);
  Address alive_app(0x7F000002, 7100);

  auto dead_owner = make_owner(0x7F000001, 7100, 1, 0);
  auto alive_owner = make_owner(0x7F000002, 7100, 2, 0);

  (void)mgr.TryCheckout(10, 1, dead_owner);
  mgr.ConfirmCheckout(10, 1);
  (void)mgr.TryCheckout(11, 1, dead_owner);
  mgr.ConfirmCheckout(11, 1);
  (void)mgr.TryCheckout(20, 1, alive_owner);
  mgr.ConfirmCheckout(20, 1);

  EXPECT_EQ(mgr.size(), 3u);

  int cleared = mgr.ClearAllFor(dead_app);
  EXPECT_EQ(cleared, 2);
  EXPECT_EQ(mgr.size(), 1u);
  EXPECT_TRUE(mgr.GetOwner(20, 1).has_value());
  EXPECT_FALSE(mgr.GetOwner(10, 1).has_value());
  EXPECT_FALSE(mgr.GetOwner(11, 1).has_value());
}

TEST(CheckoutManager, ClearAllForNoMatchReturnsZero) {
  CheckoutManager mgr;
  auto owner = make_owner(0x7F000001, 7100, 1, 100);
  (void)mgr.TryCheckout(1, 1, owner);
  mgr.ConfirmCheckout(1, 1);

  Address other(0x7F000099, 7100);
  int cleared = mgr.ClearAllFor(other);
  EXPECT_EQ(cleared, 0);
  EXPECT_EQ(mgr.size(), 1u);
}
