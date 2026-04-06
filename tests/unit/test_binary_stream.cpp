#include <gtest/gtest.h>
#include "serialization/binary_stream.hpp"

#include <cstdint>
#include <string>

using namespace atlas;

TEST(BinaryStream, TrivialTypesRoundTrip)
{
    BinaryWriter writer;
    writer.write<int8_t>(-42);
    writer.write<int16_t>(-1234);
    writer.write<int32_t>(100000);
    writer.write<int64_t>(9876543210LL);
    writer.write<float>(3.14f);
    writer.write<double>(2.718281828);

    BinaryReader reader(writer.data());

    auto v1 = reader.read<int8_t>();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, -42);

    auto v2 = reader.read<int16_t>();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, -1234);

    auto v3 = reader.read<int32_t>();
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v3, 100000);

    auto v4 = reader.read<int64_t>();
    ASSERT_TRUE(v4.has_value());
    EXPECT_EQ(*v4, 9876543210LL);

    auto v5 = reader.read<float>();
    ASSERT_TRUE(v5.has_value());
    EXPECT_NEAR(*v5, 3.14f, 1e-5f);

    auto v6 = reader.read<double>();
    ASSERT_TRUE(v6.has_value());
    EXPECT_NEAR(*v6, 2.718281828, 1e-9);
}

TEST(BinaryStream, StringRoundTrip)
{
    BinaryWriter writer;
    writer.write_string("hello atlas");

    BinaryReader reader(writer.data());
    auto s = reader.read_string();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "hello atlas");
}

TEST(BinaryStream, PackedIntSmallValue)
{
    BinaryWriter writer;
    writer.write_packed_int(100);
    EXPECT_EQ(writer.size(), 1u);

    BinaryReader reader(writer.data());
    auto v = reader.read_packed_int();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 100u);
}

TEST(BinaryStream, PackedIntLargeValue)
{
    BinaryWriter writer;
    writer.write_packed_int(300);
    EXPECT_EQ(writer.size(), 5u);

    BinaryReader reader(writer.data());
    auto v = reader.read_packed_int();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 300u);
}

TEST(BinaryStream, ReadPastEndReturnsError)
{
    BinaryWriter writer;
    writer.write<int8_t>(1);

    BinaryReader reader(writer.data());
    (void)reader.read<int8_t>();
    auto bad = reader.read<int32_t>();
    EXPECT_FALSE(bad.has_value());
    EXPECT_TRUE(reader.has_error());
}

TEST(BinaryStream, ClearAndDetach)
{
    BinaryWriter writer;
    writer.write<int32_t>(123);
    EXPECT_GT(writer.size(), 0u);

    auto buf = writer.detach();
    EXPECT_EQ(writer.size(), 0u);
    EXPECT_EQ(buf.size(), sizeof(int32_t));

    writer.write<int32_t>(456);
    writer.clear();
    EXPECT_EQ(writer.size(), 0u);
}

TEST(BinaryStream, MultipleWritesThenReads)
{
    BinaryWriter writer;
    writer.write<uint32_t>(1);
    writer.write<uint32_t>(2);
    writer.write<uint32_t>(3);
    writer.write_string("end");

    BinaryReader reader(writer.data());
    EXPECT_EQ(*reader.read<uint32_t>(), 1u);
    EXPECT_EQ(*reader.read<uint32_t>(), 2u);
    EXPECT_EQ(*reader.read<uint32_t>(), 3u);

    auto s = reader.read_string();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "end");
    EXPECT_EQ(reader.remaining(), 0u);
}

// ============================================================================
// Review issue: null pointer in write_bytes
// ============================================================================

TEST(BinaryStream, WriteBytesNullPointerDoesNotCrash)
{
    BinaryWriter writer;
    writer.write_bytes(nullptr, 0);
    EXPECT_EQ(writer.size(), 0u);

    // Non-zero size with null should also not crash (guarded)
    writer.write_bytes(nullptr, 100);
    EXPECT_EQ(writer.size(), 0u);
}

// ============================================================================
// Review issue: string read with insufficient data
// ============================================================================

TEST(BinaryStream, ReadStringInsufficientData)
{
    // Write a packed int claiming a string of length 10, but only write 3 bytes of data
    BinaryWriter writer;
    writer.write_packed_int(10);
    writer.write<uint8_t>(0x41);
    writer.write<uint8_t>(0x42);
    writer.write<uint8_t>(0x43);

    BinaryReader reader(writer.data());
    auto s = reader.read_string();
    EXPECT_FALSE(s.has_value());
    EXPECT_TRUE(reader.has_error());
}

// ============================================================================
// Review issue: empty string round-trip
// ============================================================================

TEST(BinaryStream, EmptyStringRoundTrip)
{
    BinaryWriter writer;
    writer.write_string("");

    BinaryReader reader(writer.data());
    auto s = reader.read_string();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "");
}

// ============================================================================
// Review issue: packed int boundary value (exactly 254)
// ============================================================================

TEST(BinaryStream, PackedIntBoundaryValues)
{
    BinaryWriter writer;
    writer.write_packed_int(0);
    writer.write_packed_int(254);
    writer.write_packed_int(255);
    writer.write_packed_int(256);

    // 0 → 1 byte, 254 → 1 byte, 255 → 5 bytes, 256 → 5 bytes
    EXPECT_EQ(writer.size(), 1u + 1u + 5u + 5u);

    BinaryReader reader(writer.data());
    EXPECT_EQ(*reader.read_packed_int(), 0u);
    EXPECT_EQ(*reader.read_packed_int(), 254u);
    EXPECT_EQ(*reader.read_packed_int(), 255u);
    EXPECT_EQ(*reader.read_packed_int(), 256u);
}

// ============================================================================
// Review issue: reader skip and position tracking
// ============================================================================

TEST(BinaryStream, ReaderSkipAndPosition)
{
    BinaryWriter writer;
    writer.write<int32_t>(100);
    writer.write<int32_t>(200);
    writer.write<int32_t>(300);

    BinaryReader reader(writer.data());
    EXPECT_EQ(reader.position(), 0u);

    reader.skip(4);
    EXPECT_EQ(reader.position(), 4u);

    auto v = reader.read<int32_t>();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 200);
    EXPECT_EQ(reader.position(), 8u);
    EXPECT_EQ(reader.remaining(), 4u);

    reader.reset();
    EXPECT_EQ(reader.position(), 0u);
    EXPECT_FALSE(reader.has_error());
}

// ============================================================================
// Review issue: peek does not advance position
// ============================================================================

TEST(BinaryStream, PeekDoesNotAdvance)
{
    BinaryWriter writer;
    writer.write<uint8_t>(0xAB);

    BinaryReader reader(writer.data());
    auto p = reader.peek();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::byte{0xAB});
    EXPECT_EQ(reader.position(), 0u);  // position unchanged

    // Can still read after peek
    auto v = reader.read<uint8_t>();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0xABu);
}
