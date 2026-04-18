#include <gtest/gtest.h>

#include "network/bundle.h"

using namespace atlas;

// Variable-length test message
struct BundleTestMsg {
  uint32_t value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{10, "BundleTestMsg", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint32_t>(value); }

  static auto Deserialize(BinaryReader& r) -> Result<BundleTestMsg> {
    auto v = r.Read<uint32_t>();
    if (!v) return v.Error();
    return BundleTestMsg{*v};
  }
};

// Fixed-length test message
struct FixedMsg {
  uint16_t code;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{11, "FixedMsg", MessageLengthStyle::kFixed, 2};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint16_t>(code); }

  static auto Deserialize(BinaryReader& r) -> Result<FixedMsg> {
    auto c = r.Read<uint16_t>();
    if (!c) return c.Error();
    return FixedMsg{*c};
  }
};

TEST(Bundle, EmptyBundle) {
  Bundle b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.MessageCount(), 0u);
  EXPECT_EQ(b.TotalSize(), 0u);
}

TEST(Bundle, SingleVariableMessage) {
  Bundle b;
  b.AddMessage(BundleTestMsg{42});

  EXPECT_EQ(b.MessageCount(), 1u);
  EXPECT_FALSE(b.empty());
  EXPECT_GT(b.TotalSize(), 0u);

  auto data = b.Finalize();
  EXPECT_FALSE(data.empty());
  EXPECT_TRUE(b.empty());  // finalize clears the bundle
}

TEST(Bundle, SingleFixedMessage) {
  Bundle b;
  b.AddMessage(FixedMsg{100});

  EXPECT_EQ(b.MessageCount(), 1u);
  auto data = b.Finalize();
  // Fixed: [packed ID=11 → 1 byte][uint16 code] = 3 bytes total (no length prefix)
  EXPECT_EQ(data.size(), 3u);
}

TEST(Bundle, MultipleMessages) {
  Bundle b;
  b.AddMessage(BundleTestMsg{1});
  b.AddMessage(BundleTestMsg{2});
  b.AddMessage(FixedMsg{3});

  EXPECT_EQ(b.MessageCount(), 3u);

  auto data = b.Finalize();
  EXPECT_GT(data.size(), 0u);
}

TEST(Bundle, ManualStartEndMessage) {
  Bundle b;
  MessageDesc desc{20, "Manual", MessageLengthStyle::kVariable, -1};

  b.StartMessage(desc);
  b.Writer().Write<uint32_t>(12345);
  b.EndMessage();

  EXPECT_EQ(b.MessageCount(), 1u);
  auto data = b.Finalize();
  EXPECT_FALSE(data.empty());
}

TEST(Bundle, ClearResetsState) {
  Bundle b;
  b.AddMessage(BundleTestMsg{1});
  EXPECT_EQ(b.MessageCount(), 1u);

  b.Clear();
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.MessageCount(), 0u);
  EXPECT_EQ(b.TotalSize(), 0u);
}

TEST(Bundle, HasSpaceCheck) {
  Bundle b;
  EXPECT_TRUE(b.HasSpace(100));
  EXPECT_TRUE(b.HasSpace(kMaxBundleSize));
  // After adding some data, has_space for full size should be false
  b.AddMessage(BundleTestMsg{1});
  EXPECT_FALSE(b.HasSpace(kMaxBundleSize));
}

TEST(Bundle, WireFormatVariableCanBeParsedBack) {
  // Write a variable message, then parse it back using BinaryReader
  Bundle b;
  b.AddMessage(BundleTestMsg{777});
  auto data = b.Finalize();

  BinaryReader reader{std::span<const std::byte>{data}};

  // Read packed MessageID (ID=10 fits in 1 byte)
  auto id = reader.ReadPackedInt();
  ASSERT_TRUE(id.HasValue());
  EXPECT_EQ(*id, 10u);

  // Read packed length prefix (4 fits in 1 byte)
  auto len = reader.ReadPackedInt();
  ASSERT_TRUE(len.HasValue());
  EXPECT_EQ(*len, 4u);

  // Read payload
  auto val = reader.Read<uint32_t>();
  ASSERT_TRUE(val.HasValue());
  EXPECT_EQ(*val, 777u);

  EXPECT_EQ(reader.Remaining(), 0u);
}

TEST(Bundle, WireFormatFixedCanBeParsedBack) {
  Bundle b;
  b.AddMessage(FixedMsg{999});
  auto data = b.Finalize();

  BinaryReader reader{std::span<const std::byte>{data}};

  // Read packed MessageID (ID=11 fits in 1 byte)
  auto id = reader.ReadPackedInt();
  ASSERT_TRUE(id.HasValue());
  EXPECT_EQ(*id, 11u);

  // No length prefix for fixed messages
  auto code = reader.Read<uint16_t>();
  ASSERT_TRUE(code.HasValue());
  EXPECT_EQ(*code, 999u);

  EXPECT_EQ(reader.Remaining(), 0u);
}

TEST(Bundle, FinalizeReturnsEmptyOnEmptyBundle) {
  Bundle b;
  auto data = b.Finalize();
  EXPECT_TRUE(data.empty());
  EXPECT_TRUE(b.empty());
}

// ============================================================================
// Review issue #8: Bundle writer() lifetime — writer() reference is valid
// only between start_message() and end_message(). Verify second message
// is independent.
// ============================================================================

TEST(Bundle, WriterLifetimeBetweenMessages) {
  Bundle b;
  MessageDesc desc{20, "Manual", MessageLengthStyle::kVariable, -1};

  // First message
  b.StartMessage(desc);
  auto& writer1 = b.Writer();
  writer1.Write<uint32_t>(11111);
  b.EndMessage();

  auto size_after_first = b.TotalSize();
  EXPECT_GT(size_after_first, 0u);

  // Second message — start fresh
  b.StartMessage(desc);
  auto& writer2 = b.Writer();
  writer2.Write<uint32_t>(22222);
  b.EndMessage();

  // Bundle should have 2 independent messages
  EXPECT_EQ(b.MessageCount(), 2u);
  auto total = b.TotalSize();
  EXPECT_GT(total, size_after_first);

  // Finalize and parse back both messages to verify independence
  auto data = b.Finalize();
  BinaryReader reader{std::span<const std::byte>{data}};

  // First message — packed ID and packed length
  auto id1 = reader.ReadPackedInt();
  ASSERT_TRUE(id1.HasValue());
  EXPECT_EQ(*id1, 20u);
  auto len1 = reader.ReadPackedInt();
  ASSERT_TRUE(len1.HasValue());
  EXPECT_EQ(*len1, 4u);
  auto val1 = reader.Read<uint32_t>();
  ASSERT_TRUE(val1.HasValue());
  EXPECT_EQ(*val1, 11111u);

  // Second message — packed ID and packed length
  auto id2 = reader.ReadPackedInt();
  ASSERT_TRUE(id2.HasValue());
  EXPECT_EQ(*id2, 20u);
  auto len2 = reader.ReadPackedInt();
  ASSERT_TRUE(len2.HasValue());
  EXPECT_EQ(*len2, 4u);
  auto val2 = reader.Read<uint32_t>();
  ASSERT_TRUE(val2.HasValue());
  EXPECT_EQ(*val2, 22222u);

  EXPECT_EQ(reader.Remaining(), 0u);
}

TEST(Bundle, MultipleFinalizeRoundTrips) {
  Bundle b;

  // First round
  b.AddMessage(BundleTestMsg{1});
  auto data1 = b.Finalize();
  EXPECT_FALSE(data1.empty());
  EXPECT_TRUE(b.empty());

  // Second round — reuse after finalize
  b.AddMessage(BundleTestMsg{2});
  auto data2 = b.Finalize();
  EXPECT_FALSE(data2.empty());
  EXPECT_TRUE(b.empty());
}
