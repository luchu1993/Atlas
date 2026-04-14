#include "id_client.hpp"

#include <gtest/gtest.h>

using namespace atlas;

// ============================================================================
// IDClient tests
// ============================================================================

TEST(IDClient, AllocateFromSingleRange)
{
    IDClient client;
    client.add_ids(100, 109);  // 10 IDs: well above critically_low (5)

    EXPECT_EQ(client.available(), 10u);
    EXPECT_EQ(client.allocate_id(), 100u);
    EXPECT_EQ(client.allocate_id(), 101u);
    EXPECT_EQ(client.allocate_id(), 102u);
    EXPECT_EQ(client.allocate_id(), 103u);
    EXPECT_EQ(client.allocate_id(), 104u);
    EXPECT_EQ(client.available(), 5u);

    // Allocate one more — available drops to 4 < critically_low
    EXPECT_EQ(client.allocate_id(), 105u);
    EXPECT_EQ(client.available(), 4u);

    // Now below critically_low — allocate_id() refuses
    EXPECT_TRUE(client.is_critically_low());
    EXPECT_EQ(client.allocate_id(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowRefusesAllocation)
{
    IDClient client;
    client.add_ids(1, 4);

    // 4 IDs < critically_low (5), so allocate_id() should refuse
    EXPECT_TRUE(client.is_critically_low());
    EXPECT_EQ(client.allocate_id(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowThreshold)
{
    IDClient client;
    client.add_ids(1, 5);

    // 5 IDs >= critically_low (5), so should allocate
    EXPECT_FALSE(client.is_critically_low());
    EXPECT_NE(client.allocate_id(), kInvalidEntityID);
    EXPECT_EQ(client.available(), 4u);

    // Now at 4 IDs, critically low again
    EXPECT_TRUE(client.is_critically_low());
    EXPECT_EQ(client.allocate_id(), kInvalidEntityID);
}

TEST(IDClient, NeedsRefillBelowLowWatermark)
{
    IDClient client;
    EXPECT_TRUE(client.needs_refill());  // empty

    client.add_ids(1, 63);  // 63 < low (64)
    EXPECT_TRUE(client.needs_refill());

    client.add_ids(64, 64);  // now 64 = low
    EXPECT_FALSE(client.needs_refill());
}

TEST(IDClient, IdsToRequestRespectsHighWatermark)
{
    IDClient client;
    client.add_ids(1, 1024);  // at high watermark

    EXPECT_EQ(client.ids_to_request(), 0u);
}

TEST(IDClient, IdsToRequestReturnsDesiredCount)
{
    IDClient client;
    EXPECT_EQ(client.ids_to_request(), 256u);  // kDesired
}

TEST(IDClient, MultipleRangesAreConsumedInOrder)
{
    IDClient client;
    client.add_ids(10, 14);  // 5 IDs
    client.add_ids(20, 24);  // 5 IDs
    client.add_ids(30, 30);  // 1 ID  — total 11

    EXPECT_EQ(client.available(), 11u);
    EXPECT_EQ(client.allocate_id(), 10u);
    EXPECT_EQ(client.allocate_id(), 11u);
    EXPECT_EQ(client.allocate_id(), 12u);
    EXPECT_EQ(client.allocate_id(), 13u);
    EXPECT_EQ(client.allocate_id(), 14u);
    // First range exhausted, second range starts
    EXPECT_EQ(client.allocate_id(), 20u);
    EXPECT_EQ(client.available(), 5u);

    // Allocate one more — drops below critically_low
    EXPECT_EQ(client.allocate_id(), 21u);
    EXPECT_TRUE(client.is_critically_low());
    EXPECT_EQ(client.allocate_id(), kInvalidEntityID);
}

TEST(IDClient, EmptyClientHasZeroAvailable)
{
    IDClient client;
    EXPECT_EQ(client.available(), 0u);
    EXPECT_TRUE(client.is_critically_low());
    EXPECT_EQ(client.allocate_id(), kInvalidEntityID);
}

TEST(IDClient, InvalidRangeIsIgnored)
{
    IDClient client;
    client.add_ids(10, 5);  // invalid: start > end
    EXPECT_EQ(client.available(), 0u);

    client.add_ids(kInvalidEntityID, 5);  // invalid: start == 0
    EXPECT_EQ(client.available(), 0u);
}
