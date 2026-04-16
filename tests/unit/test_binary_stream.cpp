#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "serialization/binary_stream.h"

using namespace atlas;

TEST(BinaryStream, TrivialTypesRoundTrip) {
  BinaryWriter writer;
  writer.Write<int8_t>(-42);
  writer.Write<int16_t>(-1234);
  writer.Write<int32_t>(100000);
  writer.Write<int64_t>(9876543210LL);
  writer.Write<float>(3.14f);
  writer.Write<double>(2.718281828);

  BinaryReader reader(writer.data());

  auto v1 = reader.Read<int8_t>();
  ASSERT_TRUE(v1.HasValue());
  EXPECT_EQ(*v1, -42);

  auto v2 = reader.Read<int16_t>();
  ASSERT_TRUE(v2.HasValue());
  EXPECT_EQ(*v2, -1234);

  auto v3 = reader.Read<int32_t>();
  ASSERT_TRUE(v3.HasValue());
  EXPECT_EQ(*v3, 100000);

  auto v4 = reader.Read<int64_t>();
  ASSERT_TRUE(v4.HasValue());
  EXPECT_EQ(*v4, 9876543210LL);

  auto v5 = reader.Read<float>();
  ASSERT_TRUE(v5.HasValue());
  EXPECT_NEAR(*v5, 3.14f, 1e-5f);

  auto v6 = reader.Read<double>();
  ASSERT_TRUE(v6.HasValue());
  EXPECT_NEAR(*v6, 2.718281828, 1e-9);
}

TEST(BinaryStream, StringRoundTrip) {
  BinaryWriter writer;
  writer.WriteString("hello atlas");

  BinaryReader reader(writer.data());
  auto s = reader.ReadString();
  ASSERT_TRUE(s.HasValue());
  EXPECT_EQ(*s, "hello atlas");
}

TEST(BinaryStream, PackedIntSmallValue) {
  BinaryWriter writer;
  writer.WritePackedInt(100);
  EXPECT_EQ(writer.size(), 1u);

  BinaryReader reader(writer.data());
  auto v = reader.ReadPackedInt();
  ASSERT_TRUE(v.HasValue());
  EXPECT_EQ(*v, 100u);
}

TEST(BinaryStream, PackedIntLargeValue) {
  BinaryWriter writer;
  writer.WritePackedInt(300);
  EXPECT_EQ(writer.size(), 3u);

  BinaryReader reader(writer.data());
  auto v = reader.ReadPackedInt();
  ASSERT_TRUE(v.HasValue());
  EXPECT_EQ(*v, 300u);
}

TEST(BinaryStream, ReadPastEndReturnsError) {
  BinaryWriter writer;
  writer.Write<int8_t>(1);

  BinaryReader reader(writer.data());
  (void)reader.Read<int8_t>();
  auto bad = reader.Read<int32_t>();
  EXPECT_FALSE(bad.HasValue());
  EXPECT_TRUE(reader.HasError());
}

TEST(BinaryStream, ClearAndDetach) {
  BinaryWriter writer;
  writer.Write<int32_t>(123);
  EXPECT_GT(writer.size(), 0u);

  auto buf = writer.Detach();
  EXPECT_EQ(writer.size(), 0u);
  EXPECT_EQ(buf.size(), sizeof(int32_t));

  writer.Write<int32_t>(456);
  writer.clear();
  EXPECT_EQ(writer.size(), 0u);
}

TEST(BinaryStream, MultipleWritesThenReads) {
  BinaryWriter writer;
  writer.Write<uint32_t>(1);
  writer.Write<uint32_t>(2);
  writer.Write<uint32_t>(3);
  writer.WriteString("end");

  BinaryReader reader(writer.data());
  EXPECT_EQ(*reader.Read<uint32_t>(), 1u);
  EXPECT_EQ(*reader.Read<uint32_t>(), 2u);
  EXPECT_EQ(*reader.Read<uint32_t>(), 3u);

  auto s = reader.ReadString();
  ASSERT_TRUE(s.HasValue());
  EXPECT_EQ(*s, "end");
  EXPECT_EQ(reader.Remaining(), 0u);
}

// ============================================================================
// Review issue: null pointer in write_bytes
// ============================================================================

TEST(BinaryStream, WriteBytesNullPointerDoesNotCrash) {
  BinaryWriter writer;
  writer.WriteBytes(nullptr, 0);
  EXPECT_EQ(writer.size(), 0u);

  // Non-zero size with null should also not crash (guarded)
  writer.WriteBytes(nullptr, 100);
  EXPECT_EQ(writer.size(), 0u);
}

// ============================================================================
// Review issue: string read with insufficient data
// ============================================================================

TEST(BinaryStream, ReadStringInsufficientData) {
  // Write a packed int claiming a string of length 10, but only write 3 bytes of data
  BinaryWriter writer;
  writer.WritePackedInt(10);
  writer.Write<uint8_t>(0x41);
  writer.Write<uint8_t>(0x42);
  writer.Write<uint8_t>(0x43);

  BinaryReader reader(writer.data());
  auto s = reader.ReadString();
  EXPECT_FALSE(s.HasValue());
  EXPECT_TRUE(reader.HasError());
}

// ============================================================================
// Review issue: empty string round-trip
// ============================================================================

TEST(BinaryStream, EmptyStringRoundTrip) {
  BinaryWriter writer;
  writer.WriteString("");

  BinaryReader reader(writer.data());
  auto s = reader.ReadString();
  ASSERT_TRUE(s.HasValue());
  EXPECT_EQ(*s, "");
}

// ============================================================================
// Review issue: packed int boundary value (exactly 254)
// ============================================================================

TEST(BinaryStream, PackedIntBoundaryValues) {
  BinaryWriter writer;
  writer.WritePackedInt(0);
  writer.WritePackedInt(254);
  writer.WritePackedInt(255);
  writer.WritePackedInt(256);

  // 0 → 1 byte, 254 → 3 bytes, 255 → 3 bytes, 256 → 3 bytes
  EXPECT_EQ(writer.size(), 1u + 3u + 3u + 3u);

  BinaryReader reader(writer.data());
  EXPECT_EQ(*reader.ReadPackedInt(), 0u);
  EXPECT_EQ(*reader.ReadPackedInt(), 254u);
  EXPECT_EQ(*reader.ReadPackedInt(), 255u);
  EXPECT_EQ(*reader.ReadPackedInt(), 256u);
}

// ============================================================================
// Review issue: reader skip and position tracking
// ============================================================================

TEST(BinaryStream, ReaderSkipAndPosition) {
  BinaryWriter writer;
  writer.Write<int32_t>(100);
  writer.Write<int32_t>(200);
  writer.Write<int32_t>(300);

  BinaryReader reader(writer.data());
  EXPECT_EQ(reader.Position(), 0u);

  reader.Skip(4);
  EXPECT_EQ(reader.Position(), 4u);

  auto v = reader.Read<int32_t>();
  ASSERT_TRUE(v.HasValue());
  EXPECT_EQ(*v, 200);
  EXPECT_EQ(reader.Position(), 8u);
  EXPECT_EQ(reader.Remaining(), 4u);

  reader.Reset();
  EXPECT_EQ(reader.Position(), 0u);
  EXPECT_FALSE(reader.HasError());
}

// ============================================================================
// Review issue: peek does not advance position
// ============================================================================

TEST(BinaryStream, PeekDoesNotAdvance) {
  BinaryWriter writer;
  writer.Write<uint8_t>(0xAB);

  BinaryReader reader(writer.data());
  auto p = reader.Peek();
  ASSERT_TRUE(p.HasValue());
  EXPECT_EQ(*p, std::byte{0xAB});
  EXPECT_EQ(reader.Position(), 0u);  // position unchanged

  // Can still read after peek
  auto v = reader.Read<uint8_t>();
  ASSERT_TRUE(v.HasValue());
  EXPECT_EQ(*v, 0xABu);
}
