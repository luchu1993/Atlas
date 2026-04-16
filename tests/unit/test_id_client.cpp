#include <gtest/gtest.h>

#include "id_client.h"

using namespace atlas;

// ============================================================================
// IDClient tests
// ============================================================================

TEST(IDClient, AllocateFromSingleRange) {
  IDClient client;
  client.AddIds(100, 109);  // 10 IDs: well above critically_low (5)

  EXPECT_EQ(client.available(), 10u);
  EXPECT_EQ(client.AllocateId(), 100u);
  EXPECT_EQ(client.AllocateId(), 101u);
  EXPECT_EQ(client.AllocateId(), 102u);
  EXPECT_EQ(client.AllocateId(), 103u);
  EXPECT_EQ(client.AllocateId(), 104u);
  EXPECT_EQ(client.available(), 5u);

  // Allocate one more — available drops to 4 < critically_low
  EXPECT_EQ(client.AllocateId(), 105u);
  EXPECT_EQ(client.available(), 4u);

  // Now below critically_low — allocate_id() refuses
  EXPECT_TRUE(client.is_critically_low());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowRefusesAllocation) {
  IDClient client;
  client.AddIds(1, 4);

  // 4 IDs < critically_low (5), so allocate_id() should refuse
  EXPECT_TRUE(client.is_critically_low());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowThreshold) {
  IDClient client;
  client.AddIds(1, 5);

  // 5 IDs >= critically_low (5), so should allocate
  EXPECT_FALSE(client.is_critically_low());
  EXPECT_NE(client.AllocateId(), kInvalidEntityID);
  EXPECT_EQ(client.available(), 4u);

  // Now at 4 IDs, critically low again
  EXPECT_TRUE(client.is_critically_low());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, NeedsRefillBelowLowWatermark) {
  IDClient client;
  EXPECT_TRUE(client.NeedsRefill());  // empty

  client.AddIds(1, 63);  // 63 < low (64)
  EXPECT_TRUE(client.NeedsRefill());

  client.AddIds(64, 64);  // now 64 = low
  EXPECT_FALSE(client.NeedsRefill());
}

TEST(IDClient, IdsToRequestRespectsHighWatermark) {
  IDClient client;
  client.AddIds(1, 1024);  // at high watermark

  EXPECT_EQ(client.IdsToRequest(), 0u);
}

TEST(IDClient, IdsToRequestReturnsDesiredCount) {
  IDClient client;
  EXPECT_EQ(client.IdsToRequest(), 256u);  // kDesired
}

TEST(IDClient, MultipleRangesAreConsumedInOrder) {
  IDClient client;
  client.AddIds(10, 14);  // 5 IDs
  client.AddIds(20, 24);  // 5 IDs
  client.AddIds(30, 30);  // 1 ID  — total 11

  EXPECT_EQ(client.available(), 11u);
  EXPECT_EQ(client.AllocateId(), 10u);
  EXPECT_EQ(client.AllocateId(), 11u);
  EXPECT_EQ(client.AllocateId(), 12u);
  EXPECT_EQ(client.AllocateId(), 13u);
  EXPECT_EQ(client.AllocateId(), 14u);
  // First range exhausted, second range starts
  EXPECT_EQ(client.AllocateId(), 20u);
  EXPECT_EQ(client.available(), 5u);

  // Allocate one more — drops below critically_low
  EXPECT_EQ(client.AllocateId(), 21u);
  EXPECT_TRUE(client.is_critically_low());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, EmptyClientHasZeroAvailable) {
  IDClient client;
  EXPECT_EQ(client.available(), 0u);
  EXPECT_TRUE(client.is_critically_low());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, InvalidRangeIsIgnored) {
  IDClient client;
  client.AddIds(10, 5);  // invalid: start > end
  EXPECT_EQ(client.available(), 0u);

  client.AddIds(kInvalidEntityID, 5);  // invalid: start == 0
  EXPECT_EQ(client.available(), 0u);
}
