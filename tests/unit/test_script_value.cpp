#include "script/script_value.hpp"

#include <gtest/gtest.h>

namespace atlas::test
{

TEST(ScriptValueTest, DefaultIsNone)
{
    ScriptValue v;
    EXPECT_TRUE(v.is_none());
    EXPECT_FALSE(v.is_bool());
    EXPECT_FALSE(v.is_int());
    EXPECT_FALSE(v.is_double());
    EXPECT_FALSE(v.is_string());
    EXPECT_FALSE(v.is_bytes());
    EXPECT_FALSE(v.is_object());
}

TEST(ScriptValueTest, BoolValue)
{
    ScriptValue v(true);
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());

    ScriptValue vf(false);
    EXPECT_TRUE(vf.is_bool());
    EXPECT_FALSE(vf.as_bool());
}

TEST(ScriptValueTest, IntValue)
{
    ScriptValue v(int64_t{42});
    EXPECT_TRUE(v.is_int());
    EXPECT_EQ(v.as_int(), 42);
}

TEST(ScriptValueTest, DoubleValue)
{
    ScriptValue v(3.14);
    EXPECT_TRUE(v.is_double());
    EXPECT_DOUBLE_EQ(v.as_double(), 3.14);
}

TEST(ScriptValueTest, StringValue)
{
    ScriptValue v(std::string("hello"));
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(ScriptValueTest, BytesValue)
{
    ScriptValue::Bytes data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    ScriptValue v(data);
    EXPECT_TRUE(v.is_bytes());
    EXPECT_EQ(v.as_bytes().size(), 3u);
    EXPECT_EQ(v.as_bytes()[0], std::byte{0x01});
}

TEST(ScriptValueTest, FromIntFactory)
{
    auto v = ScriptValue::from_int(7);
    EXPECT_TRUE(v.is_int());
    EXPECT_EQ(v.as_int(), 7);
}

TEST(ScriptValueTest, FromFloatFactory)
{
    auto v = ScriptValue::from_float(1.5f);
    EXPECT_TRUE(v.is_double());
    EXPECT_DOUBLE_EQ(v.as_double(), static_cast<double>(1.5f));
}

TEST(ScriptValueTest, MoveSemantics)
{
    ScriptValue a(std::string("move me"));
    ScriptValue b(std::move(a));
    EXPECT_TRUE(b.is_string());
    EXPECT_EQ(b.as_string(), "move me");
}

TEST(ScriptValueTest, CopySemantics)
{
    ScriptValue a(int64_t{99});
    ScriptValue b = a;
    EXPECT_TRUE(b.is_int());
    EXPECT_EQ(b.as_int(), 99);
}

TEST(ScriptValueTest, NegativeInt)
{
    ScriptValue v(int64_t{-1});
    EXPECT_EQ(v.as_int(), -1);
}

TEST(ScriptValueTest, EmptyString)
{
    ScriptValue v(std::string(""));
    EXPECT_TRUE(v.is_string());
    EXPECT_TRUE(v.as_string().empty());
}

}  // namespace atlas::test
