#include <gtest/gtest.h>

#include "id_client.h"

using namespace atlas;

// ============================================================================
// IDClient tests
//
// Tests reference IDClient::kCriticallyLow / kLow / kDesired / kHigh by
// name so the watermark thresholds can be retuned without rewriting the
// fixtures.
// ============================================================================

TEST(IDClient, AllocateFromSingleRange) {
  IDClient client;
  // Provision 2·kCriticallyLow IDs.  AllocateId returns invalid as soon
  // as Available drops below kCriticallyLow, so exactly
  // kCriticallyLow + 1 calls succeed before refusal — pegging the test
  // to the live constant keeps it valid through threshold retunes.
  const auto kProvisioned = 2 * IDClient::kCriticallyLow;
  const auto kSuccessfulAllocs = IDClient::kCriticallyLow + 1;
  client.AddIds(100, 100 + kProvisioned - 1);

  EXPECT_EQ(client.Available(), kProvisioned);
  for (uint64_t i = 0; i < kSuccessfulAllocs; ++i) {
    EXPECT_EQ(client.AllocateId(), 100u + i);
  }
  // Cache now sits at kCriticallyLow - 1; next call refuses.
  EXPECT_EQ(client.Available(), IDClient::kCriticallyLow - 1);
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowRefusesAllocation) {
  IDClient client;
  client.AddIds(1, IDClient::kCriticallyLow - 1);

  // Strictly below kCriticallyLow → allocate_id refuses.
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowThreshold) {
  IDClient client;
  client.AddIds(1, IDClient::kCriticallyLow);

  // Available() == kCriticallyLow → IsCriticallyLow() is false (the
  // threshold is `< kCriticallyLow`, exclusive), so one allocation
  // succeeds before crossing.
  EXPECT_FALSE(client.IsCriticallyLow());
  EXPECT_NE(client.AllocateId(), kInvalidEntityID);
  EXPECT_EQ(client.Available(), IDClient::kCriticallyLow - 1);

  // Now strictly below kCriticallyLow.
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, NeedsRefillBelowLowWatermark) {
  IDClient client;
  EXPECT_TRUE(client.NeedsRefill());  // empty

  client.AddIds(1, IDClient::kLow - 1);
  EXPECT_TRUE(client.NeedsRefill());

  client.AddIds(IDClient::kLow, IDClient::kLow);  // now exactly kLow
  EXPECT_FALSE(client.NeedsRefill());
}

TEST(IDClient, IdsToRequestRespectsHighWatermark) {
  IDClient client;
  client.AddIds(1, IDClient::kHigh);  // at high watermark

  EXPECT_EQ(client.IdsToRequest(), 0u);
}

TEST(IDClient, IdsToRequestReturnsDesiredCount) {
  IDClient client;
  EXPECT_EQ(client.IdsToRequest(), IDClient::kDesired);
}

TEST(IDClient, MultipleRangesAreConsumedInOrder) {
  // Provision three non-contiguous ranges of n IDs each, where n is
  // sized off kCriticallyLow so the test stays valid when the threshold
  // is retuned.  Drain all 3·n IDs and verify each came back in
  // declaration order across range boundaries.
  IDClient client;
  const auto n = IDClient::kCriticallyLow;  // ≥ 16 IDs per range
  client.AddIds(100, 100 + n - 1);
  client.AddIds(200, 200 + n - 1);
  client.AddIds(300, 300 + n - 1);
  EXPECT_EQ(client.Available(), 3 * n);

  for (uint64_t i = 0; i < n; ++i) EXPECT_EQ(client.AllocateId(), 100u + i);
  for (uint64_t i = 0; i < n; ++i) EXPECT_EQ(client.AllocateId(), 200u + i);
  // Third range only drains while Available stays at-or-above the
  // refusal threshold — AllocateId returns invalid past that point.
  uint64_t third_idx = 0;
  while (!client.IsCriticallyLow()) {
    EXPECT_EQ(client.AllocateId(), 300u + third_idx);
    ++third_idx;
  }
  // Cache holds kCriticallyLow - 1 IDs after the loop (the alloc that
  // dropped Available below the threshold ran one tick before the
  // condition was re-checked).
  EXPECT_EQ(client.Available(), IDClient::kCriticallyLow - 1);
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, EmptyClientHasZeroAvailable) {
  IDClient client;
  EXPECT_EQ(client.Available(), 0u);
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, InvalidRangeIsIgnored) {
  IDClient client;
  client.AddIds(10, 5);  // invalid: start > end
  EXPECT_EQ(client.Available(), 0u);

  client.AddIds(kInvalidEntityID, 5);  // invalid: start == 0
  EXPECT_EQ(client.Available(), 0u);
}
