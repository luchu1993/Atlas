#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "network/compression_filter.h"
#include "network/packet_filter.h"

using namespace atlas;

namespace {

auto make_bytes(std::size_t n, std::byte fill = std::byte{0xAB}) -> std::vector<std::byte> {
  return std::vector<std::byte>(n, fill);
}

auto make_compressible_bytes(std::size_t n) -> std::vector<std::byte> {
  std::vector<std::byte> data(n);
  for (std::size_t i = 0; i < n; ++i) {
    data[i] = static_cast<std::byte>(i % 4);
  }
  return data;
}

}  // namespace

TEST(PacketFilterTest, BasePassthroughSend) {
  PacketFilter filter;
  auto data = make_bytes(100);
  auto result = filter.SendFilter(data);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(*result, data);
}

TEST(PacketFilterTest, BasePassthroughRecv) {
  PacketFilter filter;
  auto data = make_bytes(100);
  auto result = filter.RecvFilter(data);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(*result, data);
}

TEST(PacketFilterTest, BaseZeroOverhead) {
  PacketFilter filter;
  EXPECT_EQ(filter.MaxOverhead(), 0u);
}

TEST(CompressionFilterTest, SmallPacketPassthrough) {
  CompressionFilter filter(256);
  auto data = make_bytes(100);

  auto sent = filter.SendFilter(data);
  ASSERT_TRUE(sent.HasValue());
  EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 0u);
  EXPECT_EQ(sent->size(), 1u + data.size());

  auto received = filter.RecvFilter(*sent);
  ASSERT_TRUE(received.HasValue());
  EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, LargePacketCompressed) {
  CompressionFilter filter(64);
  auto data = make_compressible_bytes(1024);

  auto sent = filter.SendFilter(data);
  ASSERT_TRUE(sent.HasValue());
  EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 1u);
  EXPECT_LT(sent->size(), data.size());

  auto received = filter.RecvFilter(*sent);
  ASSERT_TRUE(received.HasValue());
  EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, IncompressibleDataFallback) {
  CompressionFilter filter(4);

  std::vector<std::byte> random_data(512);
  for (std::size_t i = 0; i < random_data.size(); ++i) {
    random_data[i] = static_cast<std::byte>((i * 7 + 13) % 256);
  }

  auto sent = filter.SendFilter(random_data);
  ASSERT_TRUE(sent.HasValue());

  auto received = filter.RecvFilter(*sent);
  ASSERT_TRUE(received.HasValue());
  EXPECT_EQ(*received, random_data);
}

TEST(CompressionFilterTest, EmptyPacket) {
  CompressionFilter filter(256);
  std::vector<std::byte> empty;

  auto sent = filter.SendFilter(empty);
  ASSERT_TRUE(sent.HasValue());
  EXPECT_EQ(sent->size(), 1u);
  EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 0u);

  auto received = filter.RecvFilter(*sent);
  ASSERT_TRUE(received.HasValue());
  EXPECT_TRUE(received->empty());
}

TEST(CompressionFilterTest, RecvEmptyReturnsError) {
  CompressionFilter filter(256);
  std::span<const std::byte> empty;

  auto result = filter.RecvFilter(empty);
  EXPECT_FALSE(result.HasValue());
}

TEST(CompressionFilterTest, RecvTruncatedDeflateHeaderReturnsError) {
  CompressionFilter filter(256);
  std::vector<std::byte> truncated = {static_cast<std::byte>(1), static_cast<std::byte>(0)};

  auto result = filter.RecvFilter(truncated);
  EXPECT_FALSE(result.HasValue());
}

TEST(CompressionFilterTest, MaxOverhead) {
  CompressionFilter filter;
  EXPECT_EQ(filter.MaxOverhead(), 5u);
}

TEST(CompressionFilterTest, RoundtripLargeData) {
  CompressionFilter filter(128);
  auto data = make_compressible_bytes(65000);

  auto sent = filter.SendFilter(data);
  ASSERT_TRUE(sent.HasValue());

  auto received = filter.RecvFilter(*sent);
  ASSERT_TRUE(received.HasValue());
  EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, CompressionTypeNone) {
  CompressionFilter filter(64, CompressionType::kNone);
  auto data = make_compressible_bytes(1024);

  auto sent = filter.SendFilter(data);
  ASSERT_TRUE(sent.HasValue());
  EXPECT_EQ(static_cast<uint8_t>((*sent)[0]), 0u);
  EXPECT_EQ(sent->size(), 1u + data.size());

  auto received = filter.RecvFilter(*sent);
  ASSERT_TRUE(received.HasValue());
  EXPECT_EQ(*received, data);
}

TEST(CompressionFilterTest, ThresholdBoundary) {
  CompressionFilter filter(256);
  auto data_below = make_compressible_bytes(255);
  auto data_at = make_compressible_bytes(256);
  auto data_above = make_compressible_bytes(257);

  auto sent_below = filter.SendFilter(data_below);
  ASSERT_TRUE(sent_below.HasValue());
  EXPECT_EQ(static_cast<uint8_t>((*sent_below)[0]), 0u);

  auto sent_at = filter.SendFilter(data_at);
  ASSERT_TRUE(sent_at.HasValue());
  EXPECT_EQ(static_cast<uint8_t>((*sent_at)[0]), 1u);

  auto sent_above = filter.SendFilter(data_above);
  ASSERT_TRUE(sent_above.HasValue());
  EXPECT_EQ(static_cast<uint8_t>((*sent_above)[0]), 1u);

  // All round-trip correctly
  auto r1 = filter.RecvFilter(*sent_below);
  ASSERT_TRUE(r1.HasValue());
  EXPECT_EQ(*r1, data_below);

  auto r2 = filter.RecvFilter(*sent_at);
  ASSERT_TRUE(r2.HasValue());
  EXPECT_EQ(*r2, data_at);

  auto r3 = filter.RecvFilter(*sent_above);
  ASSERT_TRUE(r3.HasValue());
  EXPECT_EQ(*r3, data_above);
}
