#include <gtest/gtest.h>
#include "network/message.hpp"

using namespace atlas;

// Define a test message satisfying NetworkMessage concept
struct TestMsg
{
    uint32_t value;
    std::string text;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1, "TestMsg", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(value);
        w.write_string(text);
    }

    static auto deserialize(BinaryReader& r) -> Result<TestMsg>
    {
        auto v = r.read<uint32_t>();
        if (!v) return v.error();
        auto t = r.read_string();
        if (!t) return t.error();
        return TestMsg{*v, std::move(*t)};
    }
};

// Verify concept satisfaction
static_assert(NetworkMessage<TestMsg>);

// Fixed-length test message
struct PingMsg
{
    uint32_t sequence;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{2, "PingMsg", MessageLengthStyle::Fixed, 4};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint32_t>(sequence); }

    static auto deserialize(BinaryReader& r) -> Result<PingMsg>
    {
        auto s = r.read<uint32_t>();
        if (!s) return s.error();
        return PingMsg{*s};
    }
};

static_assert(NetworkMessage<PingMsg>);

TEST(Message, ConceptSatisfied)
{
    // compile-time check already done by static_assert above
    EXPECT_EQ(TestMsg::descriptor().id, 1);
    EXPECT_EQ(TestMsg::descriptor().name, "TestMsg");
    EXPECT_FALSE(TestMsg::descriptor().is_fixed());

    EXPECT_EQ(PingMsg::descriptor().id, 2);
    EXPECT_TRUE(PingMsg::descriptor().is_fixed());
}

TEST(Message, SerializeDeserializeRoundTrip)
{
    TestMsg orig{42, "hello"};
    BinaryWriter w;
    orig.serialize(w);

    BinaryReader r(w.data());
    auto result = TestMsg::deserialize(r);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 42u);
    EXPECT_EQ(result->text, "hello");
}

TEST(Message, FixedMessageRoundTrip)
{
    PingMsg orig{12345};
    BinaryWriter w;
    orig.serialize(w);

    EXPECT_EQ(w.size(), sizeof(uint32_t));

    BinaryReader r(w.data());
    auto result = PingMsg::deserialize(r);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sequence, 12345u);
    EXPECT_EQ(r.remaining(), 0u);
}

TEST(Message, TypedHandlerDispatch)
{
    bool called = false;
    uint32_t received_value = 0;

    auto handler = make_handler<TestMsg>(
        [&](const Address&, Channel*, const TestMsg& msg)
        {
            called = true;
            received_value = msg.value;
        });

    // Serialize a TestMsg
    TestMsg msg{99, "world"};
    BinaryWriter w;
    msg.serialize(w);

    BinaryReader r(w.data());
    handler->handle_message(Address::NONE, nullptr, 1, r);

    EXPECT_TRUE(called);
    EXPECT_EQ(received_value, 99u);
}

TEST(Message, TypedHandlerMalformedData)
{
    bool called = false;
    auto handler = make_handler<TestMsg>(
        [&](const Address&, Channel*, const TestMsg&)
        {
            called = true;
        });

    // Feed garbage data — too short for TestMsg
    BinaryWriter w;
    w.write<uint8_t>(0xFF);

    BinaryReader r(w.data());
    handler->handle_message(Address::NONE, nullptr, 1, r);

    // Handler should not be called (deserialization failed silently)
    EXPECT_FALSE(called);
}

TEST(Message, EmptyStringRoundTrip)
{
    TestMsg orig{0, ""};
    BinaryWriter w;
    orig.serialize(w);

    BinaryReader r(w.data());
    auto result = TestMsg::deserialize(r);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 0u);
    EXPECT_EQ(result->text, "");
}

TEST(Message, LargePayloadRoundTrip)
{
    std::string big(4096, 'X');
    TestMsg orig{0xDEADBEEF, big};
    BinaryWriter w;
    orig.serialize(w);

    BinaryReader r(w.data());
    auto result = TestMsg::deserialize(r);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 0xDEADBEEFu);
    EXPECT_EQ(result->text, big);
}

TEST(Message, MessageDescIsFixedHelper)
{
    MessageDesc variable{10, "Var", MessageLengthStyle::Variable, -1};
    MessageDesc fixed{11, "Fix", MessageLengthStyle::Fixed, 8};

    EXPECT_FALSE(variable.is_fixed());
    EXPECT_TRUE(fixed.is_fixed());
}
