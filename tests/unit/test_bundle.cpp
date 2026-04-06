#include <gtest/gtest.h>
#include "network/bundle.hpp"

using namespace atlas;

// Variable-length test message
struct BundleTestMsg
{
    uint32_t value;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{10, "BundleTestMsg", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint32_t>(value); }

    static auto deserialize(BinaryReader& r) -> Result<BundleTestMsg>
    {
        auto v = r.read<uint32_t>();
        if (!v) return v.error();
        return BundleTestMsg{*v};
    }
};

// Fixed-length test message
struct FixedMsg
{
    uint16_t code;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{11, "FixedMsg", MessageLengthStyle::Fixed, 2};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint16_t>(code); }

    static auto deserialize(BinaryReader& r) -> Result<FixedMsg>
    {
        auto c = r.read<uint16_t>();
        if (!c) return c.error();
        return FixedMsg{*c};
    }
};

TEST(Bundle, EmptyBundle)
{
    Bundle b;
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.message_count(), 0u);
    EXPECT_EQ(b.total_size(), 0u);
}

TEST(Bundle, SingleVariableMessage)
{
    Bundle b;
    b.add_message(BundleTestMsg{42});

    EXPECT_EQ(b.message_count(), 1u);
    EXPECT_FALSE(b.empty());
    EXPECT_GT(b.total_size(), 0u);

    auto data = b.finalize();
    EXPECT_FALSE(data.empty());
    EXPECT_TRUE(b.empty());  // finalize clears the bundle
}

TEST(Bundle, SingleFixedMessage)
{
    Bundle b;
    b.add_message(FixedMsg{100});

    EXPECT_EQ(b.message_count(), 1u);
    auto data = b.finalize();
    // Fixed: [uint16 ID][uint16 code] = 4 bytes total (no length prefix)
    EXPECT_EQ(data.size(), 4u);
}

TEST(Bundle, MultipleMessages)
{
    Bundle b;
    b.add_message(BundleTestMsg{1});
    b.add_message(BundleTestMsg{2});
    b.add_message(FixedMsg{3});

    EXPECT_EQ(b.message_count(), 3u);

    auto data = b.finalize();
    EXPECT_GT(data.size(), 0u);
}

TEST(Bundle, ManualStartEndMessage)
{
    Bundle b;
    MessageDesc desc{20, "Manual", MessageLengthStyle::Variable, -1};

    b.start_message(desc);
    b.writer().write<uint32_t>(12345);
    b.end_message();

    EXPECT_EQ(b.message_count(), 1u);
    auto data = b.finalize();
    EXPECT_FALSE(data.empty());
}

TEST(Bundle, ClearResetsState)
{
    Bundle b;
    b.add_message(BundleTestMsg{1});
    EXPECT_EQ(b.message_count(), 1u);

    b.clear();
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.message_count(), 0u);
    EXPECT_EQ(b.total_size(), 0u);
}

TEST(Bundle, HasSpaceCheck)
{
    Bundle b;
    EXPECT_TRUE(b.has_space(100));
    EXPECT_TRUE(b.has_space(kMaxBundleSize));
    // After adding some data, has_space for full size should be false
    b.add_message(BundleTestMsg{1});
    EXPECT_FALSE(b.has_space(kMaxBundleSize));
}

TEST(Bundle, WireFormatVariableCanBeParsedBack)
{
    // Write a variable message, then parse it back using BinaryReader
    Bundle b;
    b.add_message(BundleTestMsg{777});
    auto data = b.finalize();

    BinaryReader reader{std::span<const std::byte>{data}};

    // Read MessageID
    auto id = reader.read<uint16_t>();
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 10u);  // BundleTestMsg::descriptor().id

    // Read packed int length
    auto len = reader.read_packed_int();
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 4u);  // sizeof(uint32_t)

    // Read payload
    auto val = reader.read<uint32_t>();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 777u);

    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(Bundle, WireFormatFixedCanBeParsedBack)
{
    Bundle b;
    b.add_message(FixedMsg{999});
    auto data = b.finalize();

    BinaryReader reader{std::span<const std::byte>{data}};

    auto id = reader.read<uint16_t>();
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 11u);  // FixedMsg::descriptor().id

    // No length prefix for fixed messages
    auto code = reader.read<uint16_t>();
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 999u);

    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(Bundle, FinalizeReturnsEmptyOnEmptyBundle)
{
    Bundle b;
    auto data = b.finalize();
    EXPECT_TRUE(data.empty());
    EXPECT_TRUE(b.empty());
}

TEST(Bundle, MultipleFinalizeRoundTrips)
{
    Bundle b;

    // First round
    b.add_message(BundleTestMsg{1});
    auto data1 = b.finalize();
    EXPECT_FALSE(data1.empty());
    EXPECT_TRUE(b.empty());

    // Second round — reuse after finalize
    b.add_message(BundleTestMsg{2});
    auto data2 = b.finalize();
    EXPECT_FALSE(data2.empty());
    EXPECT_TRUE(b.empty());
}
