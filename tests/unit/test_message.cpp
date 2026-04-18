#include <gtest/gtest.h>

#include "network/message.h"

using namespace atlas;

// Define a test message satisfying NetworkMessage concept
struct TestMsg {
  uint32_t value;
  std::string text;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{1, "TestMsg", MessageLengthStyle::kVariable, -1};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(value);
    w.WriteString(text);
  }

  static auto Deserialize(BinaryReader& r) -> Result<TestMsg> {
    auto v = r.Read<uint32_t>();
    if (!v) return v.Error();
    auto t = r.ReadString();
    if (!t) return t.Error();
    return TestMsg{*v, std::move(*t)};
  }
};

// Verify concept satisfaction
static_assert(NetworkMessage<TestMsg>);

// Fixed-length test message
struct PingMsg {
  uint32_t sequence;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{2, "PingMsg", MessageLengthStyle::kFixed, 4};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint32_t>(sequence); }

  static auto Deserialize(BinaryReader& r) -> Result<PingMsg> {
    auto s = r.Read<uint32_t>();
    if (!s) return s.Error();
    return PingMsg{*s};
  }
};

static_assert(NetworkMessage<PingMsg>);

TEST(Message, ConceptSatisfied) {
  // compile-time check already done by static_assert above
  EXPECT_EQ(TestMsg::Descriptor().id, 1);
  EXPECT_EQ(TestMsg::Descriptor().name, "TestMsg");
  EXPECT_FALSE(TestMsg::Descriptor().IsFixed());

  EXPECT_EQ(PingMsg::Descriptor().id, 2);
  EXPECT_TRUE(PingMsg::Descriptor().IsFixed());
}

TEST(Message, SerializeDeserializeRoundTrip) {
  TestMsg orig{42, "hello"};
  BinaryWriter w;
  orig.Serialize(w);

  BinaryReader r(w.Data());
  auto result = TestMsg::Deserialize(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->value, 42u);
  EXPECT_EQ(result->text, "hello");
}

TEST(Message, FixedMessageRoundTrip) {
  PingMsg orig{12345};
  BinaryWriter w;
  orig.Serialize(w);

  EXPECT_EQ(w.Size(), sizeof(uint32_t));

  BinaryReader r(w.Data());
  auto result = PingMsg::Deserialize(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->sequence, 12345u);
  EXPECT_EQ(r.Remaining(), 0u);
}

TEST(Message, TypedHandlerDispatch) {
  bool called = false;
  uint32_t received_value = 0;

  auto handler = MakeHandler<TestMsg>([&](const Address&, Channel*, const TestMsg& msg) {
    called = true;
    received_value = msg.value;
  });

  // Serialize a TestMsg
  TestMsg msg{99, "world"};
  BinaryWriter w;
  msg.Serialize(w);

  BinaryReader r(w.Data());
  handler->HandleMessage(Address::kNone, nullptr, 1, r);

  EXPECT_TRUE(called);
  EXPECT_EQ(received_value, 99u);
}

TEST(Message, TypedHandlerMalformedData) {
  bool called = false;
  auto handler =
      MakeHandler<TestMsg>([&](const Address&, Channel*, const TestMsg&) { called = true; });

  // Feed garbage data — too short for TestMsg
  BinaryWriter w;
  w.Write<uint8_t>(0xFF);

  BinaryReader r(w.Data());
  handler->HandleMessage(Address::kNone, nullptr, 1, r);

  // Handler should not be called (deserialization failed silently)
  EXPECT_FALSE(called);
}

TEST(Message, EmptyStringRoundTrip) {
  TestMsg orig{0, ""};
  BinaryWriter w;
  orig.Serialize(w);

  BinaryReader r(w.Data());
  auto result = TestMsg::Deserialize(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->value, 0u);
  EXPECT_EQ(result->text, "");
}

TEST(Message, LargePayloadRoundTrip) {
  std::string big(4096, 'X');
  TestMsg orig{0xDEADBEEF, big};
  BinaryWriter w;
  orig.Serialize(w);

  BinaryReader r(w.Data());
  auto result = TestMsg::Deserialize(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->value, 0xDEADBEEFu);
  EXPECT_EQ(result->text, big);
}

TEST(Message, MessageDescIsFixedHelper) {
  MessageDesc variable{10, "Var", MessageLengthStyle::kVariable, -1};
  MessageDesc fixed{11, "Fix", MessageLengthStyle::kFixed, 8};

  EXPECT_FALSE(variable.IsFixed());
  EXPECT_TRUE(fixed.IsFixed());
}

// ---------------------------------------------------------------------------
// MessageReliability
// ---------------------------------------------------------------------------

// Default reliability is Reliable when not specified.
struct ReliableMsg {
  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{20, "ReliableMsg", MessageLengthStyle::kFixed, 0};
    return desc;
  }
  void Serialize(BinaryWriter&) const {}
  static auto Deserialize(BinaryReader&) -> Result<ReliableMsg> { return ReliableMsg{}; }
};
static_assert(NetworkMessage<ReliableMsg>);

// Explicitly marked unreliable.
struct UnreliableMsg {
  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{21, "UnreliableMsg", MessageLengthStyle::kFixed, 0,
                                  MessageReliability::kUnreliable};
    return desc;
  }
  void Serialize(BinaryWriter&) const {}
  static auto Deserialize(BinaryReader&) -> Result<UnreliableMsg> { return UnreliableMsg{}; }
};
static_assert(NetworkMessage<UnreliableMsg>);

TEST(Message, DefaultReliabilityIsReliable) {
  EXPECT_EQ(ReliableMsg::Descriptor().reliability, MessageReliability::kReliable);
  EXPECT_FALSE(ReliableMsg::Descriptor().IsUnreliable());
}

TEST(Message, ExplicitUnreliableFlag) {
  EXPECT_EQ(UnreliableMsg::Descriptor().reliability, MessageReliability::kUnreliable);
  EXPECT_TRUE(UnreliableMsg::Descriptor().IsUnreliable());
}

TEST(Message, DescConstructorDefaultReliability) {
  // Four-argument constructor keeps backward compatibility — reliability defaults to Reliable.
  MessageDesc desc{30, "Compat", MessageLengthStyle::kVariable, -1};
  EXPECT_FALSE(desc.IsUnreliable());
}

TEST(Message, DescConstructorExplicitReliability) {
  MessageDesc desc{31, "Unreliable", MessageLengthStyle::kVariable, -1,
                   MessageReliability::kUnreliable};
  EXPECT_TRUE(desc.IsUnreliable());
}
