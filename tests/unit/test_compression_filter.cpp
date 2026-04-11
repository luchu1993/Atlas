#include "network/compression_filter.hpp"
#include "network/packet_filter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

using namespace atlas;

namespace
{

auto make_bytes(std::size_t n, std::byte fill = std::byte{0xAB}) -> std::vector<std::byte>
{
    return std::vector<std::byte>(n, fill);
}

auto make_compressible_bytes(std::size_t n) -> std::vector<std::byte>
{
    std::vector<std::byte> data(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        data[i] = static_cast<std::byte>(i % 4);
    }
    return data;
}

}  // namespace

TEST(PacketFilterTest, BasePassthroughSend)
{
    PacketFilter filter;
    auto data = make_bytes(100);
    auto result = filter.send_filter(data);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, data);
}

TEST(PacketFilterTest, BasePassthroughRecv)
{
    PacketFilter filter;
    auto data = make_bytes(100);
    auto result = filter.recv_filter(data);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, data);
}

TEST(PacketFilterTest, BaseZeroOverhead)
{
    PacketFilter filter;
    EXPECT_EQ(filter.max_overhead(), 0u);
}

TEST(CompressionFilterTest, SmallPacketPassthrough)
{
    CompressionFilter filter(256);
    auto data = make_bytes(100);

    auto sent = filter.send_filter(data);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 0u);
    EXPECT_EQ(sent->size(), 1u + data.size());

    auto received = filter.recv_filter(*sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, LargePacketCompressed)
{
    CompressionFilter filter(64);
    auto data = make_compressible_bytes(1024);

    auto sent = filter.send_filter(data);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 1u);
    EXPECT_LT(sent->size(), data.size());

    auto received = filter.recv_filter(*sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, IncompressibleDataFallback)
{
    CompressionFilter filter(4);

    std::vector<std::byte> random_data(512);
    for (std::size_t i = 0; i < random_data.size(); ++i)
    {
        random_data[i] = static_cast<std::byte>((i * 7 + 13) % 256);
    }

    auto sent = filter.send_filter(random_data);
    ASSERT_TRUE(sent.has_value());

    auto received = filter.recv_filter(*sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, random_data);
}

TEST(CompressionFilterTest, EmptyPacket)
{
    CompressionFilter filter(256);
    std::vector<std::byte> empty;

    auto sent = filter.send_filter(empty);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(sent->size(), 1u);
    EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 0u);

    auto received = filter.recv_filter(*sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_TRUE(received->empty());
}

TEST(CompressionFilterTest, RecvEmptyReturnsError)
{
    CompressionFilter filter(256);
    std::span<const std::byte> empty;

    auto result = filter.recv_filter(empty);
    EXPECT_FALSE(result.has_value());
}

TEST(CompressionFilterTest, RecvTruncatedDeflateHeaderReturnsError)
{
    CompressionFilter filter(256);
    std::vector<std::byte> truncated = {static_cast<std::byte>(1), static_cast<std::byte>(0)};

    auto result = filter.recv_filter(truncated);
    EXPECT_FALSE(result.has_value());
}

TEST(CompressionFilterTest, MaxOverhead)
{
    CompressionFilter filter;
    EXPECT_EQ(filter.max_overhead(), 5u);
}

TEST(CompressionFilterTest, RoundtripLargeData)
{
    CompressionFilter filter(128);
    auto data = make_compressible_bytes(65000);

    auto sent = filter.send_filter(data);
    ASSERT_TRUE(sent.has_value());

    auto received = filter.recv_filter(*sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, CompressionTypeNone)
{
    CompressionFilter filter(64, CompressionType::None);
    auto data = make_compressible_bytes(1024);

    auto sent = filter.send_filter(data);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 0u);
    EXPECT_EQ(sent->size(), 1u + data.size());

    auto received = filter.recv_filter(*sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, ThresholdBoundary)
{
    CompressionFilter filter(256);
    auto data_below = make_compressible_bytes(255);
    auto data_at = make_compressible_bytes(256);
    auto data_above = make_compressible_bytes(257);

    auto sent_below = filter.send_filter(data_below);
    ASSERT_TRUE(sent_below.has_value());
    EXPECT_EQ(static_cast<uint8_t>((*sent_below)[0]), 0u);

    auto sent_at = filter.send_filter(data_at);
    ASSERT_TRUE(sent_at.has_value());
    EXPECT_EQ(static_cast<uint8_t>((*sent_at)[0]), 1u);

    auto sent_above = filter.send_filter(data_above);
    ASSERT_TRUE(sent_above.has_value());
    EXPECT_EQ(static_cast<uint8_t>((*sent_above)[0]), 1u);

    // All round-trip correctly
    auto r1 = filter.recv_filter(*sent_below);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, data_below);

    auto r2 = filter.recv_filter(*sent_at);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, data_at);

    auto r3 = filter.recv_filter(*sent_above);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, data_above);
}
