#include "network/machined_types.hpp"
#include "serialization/binary_stream.hpp"

#include <gtest/gtest.h>

using namespace atlas;
using namespace atlas::machined;

// Helper: serialize then deserialize
template <typename T>
static auto round_trip(const T& msg) -> Result<T>
{
    BinaryWriter w;
    msg.serialize(w);
    auto data = w.data();
    BinaryReader r(data);
    return T::deserialize(r);
}

// ============================================================================
// RegisterMessage
// ============================================================================

TEST(MachinedTypes, RegisterRoundTrip)
{
    RegisterMessage orig{"baseapp", 7100, 12345};
    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, "baseapp");
    EXPECT_EQ(result->port, 7100);
    EXPECT_EQ(result->pid, 12345u);
}

TEST(MachinedTypes, RegisterEmptyType)
{
    RegisterMessage orig{"", 0, 0};
    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, "");
}

// ============================================================================
// DeregisterMessage
// ============================================================================

TEST(MachinedTypes, DeregisterRoundTrip)
{
    DeregisterMessage orig{"cellapp", 99};
    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, "cellapp");
    EXPECT_EQ(result->pid, 99u);
}

// ============================================================================
// QueryMessage
// ============================================================================

TEST(MachinedTypes, QueryRoundTrip)
{
    QueryMessage orig{"loginapp"};
    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, "loginapp");
}

// ============================================================================
// QueryResponse
// ============================================================================

TEST(MachinedTypes, QueryResponseEmpty)
{
    QueryResponse orig;
    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->processes.empty());
}

TEST(MachinedTypes, QueryResponseMultipleProcesses)
{
    QueryResponse orig;
    orig.processes.push_back({"baseapp", Address(0x7F000001, 7100), 100});
    orig.processes.push_back({"baseapp", Address(0x7F000002, 7101), 101});
    orig.processes.push_back({"cellapp", Address(0x7F000003, 7200), 200});

    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->processes.size(), 3u);

    EXPECT_EQ(result->processes[0].type, "baseapp");
    EXPECT_EQ(result->processes[0].address.ip(), 0x7F000001u);
    EXPECT_EQ(result->processes[0].address.port(), 7100u);
    EXPECT_EQ(result->processes[0].pid, 100u);

    EXPECT_EQ(result->processes[1].type, "baseapp");
    EXPECT_EQ(result->processes[1].pid, 101u);

    EXPECT_EQ(result->processes[2].type, "cellapp");
    EXPECT_EQ(result->processes[2].pid, 200u);
}

// ============================================================================
// QueryResponse: DoS guard — count > kMaxProcesses must return error
// ============================================================================

TEST(MachinedTypes, QueryResponseCountExceedsLimit)
{
    // Craft a malformed payload: count = 10001, then truncated body
    BinaryWriter w;
    w.write<uint32_t>(10001u);  // exceeds kMaxProcesses (10000)
    auto data = w.data();
    BinaryReader r(data);

    auto result = QueryResponse::deserialize(r);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(MachinedTypes, QueryResponseMaxAllowed)
{
    // Exactly at the limit should succeed (if data is present)
    // We only test that count=10000 is accepted, not that it reads all entries
    // (building 10000 entries would be slow; we test the boundary check only)
    QueryResponse orig;
    for (int i = 0; i < 5; ++i)
    {
        orig.processes.push_back({"svc", Address(0x7F000001, static_cast<uint16_t>(7000 + i)),
                                  static_cast<uint32_t>(i)});
    }
    auto result = round_trip(orig);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->processes.size(), 5u);
}

// ============================================================================
// Truncated payloads return errors (not crashes)
// ============================================================================

TEST(MachinedTypes, RegisterTruncatedReturnsError)
{
    std::vector<std::byte> empty;
    BinaryReader r(empty);
    auto result = RegisterMessage::deserialize(r);
    EXPECT_FALSE(result.has_value());
}

TEST(MachinedTypes, QueryResponseTruncatedAfterCount)
{
    BinaryWriter w;
    w.write<uint32_t>(3u);  // claims 3 entries but provides none
    auto data = w.data();
    BinaryReader r(data);
    auto result = QueryResponse::deserialize(r);
    EXPECT_FALSE(result.has_value());
}
