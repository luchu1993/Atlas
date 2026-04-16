#include <gtest/gtest.h>

#include "script/script_value.h"

namespace atlas::test {

TEST(ScriptValueTest, DefaultIsNone) {
  ScriptValue v;
  EXPECT_TRUE(v.IsNone());
  EXPECT_FALSE(v.IsBool());
  EXPECT_FALSE(v.IsInt());
  EXPECT_FALSE(v.IsDouble());
  EXPECT_FALSE(v.IsString());
  EXPECT_FALSE(v.IsBytes());
  EXPECT_FALSE(v.IsObject());
}

TEST(ScriptValueTest, BoolValue) {
  ScriptValue v(true);
  EXPECT_TRUE(v.IsBool());
  EXPECT_TRUE(v.AsBool());

  ScriptValue vf(false);
  EXPECT_TRUE(vf.IsBool());
  EXPECT_FALSE(vf.AsBool());
}

TEST(ScriptValueTest, IntValue) {
  ScriptValue v(int64_t{42});
  EXPECT_TRUE(v.IsInt());
  EXPECT_EQ(v.AsInt(), 42);
}

TEST(ScriptValueTest, DoubleValue) {
  ScriptValue v(3.14);
  EXPECT_TRUE(v.IsDouble());
  EXPECT_DOUBLE_EQ(v.AsDouble(), 3.14);
}

TEST(ScriptValueTest, StringValue) {
  ScriptValue v(std::string("hello"));
  EXPECT_TRUE(v.IsString());
  EXPECT_EQ(v.AsString(), "hello");
}

TEST(ScriptValueTest, BytesValue) {
  ScriptValue::Bytes data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  ScriptValue v(data);
  EXPECT_TRUE(v.IsBytes());
  EXPECT_EQ(v.AsBytes().size(), 3u);
  EXPECT_EQ(v.AsBytes()[0], std::byte{0x01});
}

TEST(ScriptValueTest, FromIntFactory) {
  auto v = ScriptValue::FromInt(7);
  EXPECT_TRUE(v.IsInt());
  EXPECT_EQ(v.AsInt(), 7);
}

TEST(ScriptValueTest, FromFloatFactory) {
  auto v = ScriptValue::FromFloat(1.5f);
  EXPECT_TRUE(v.IsDouble());
  EXPECT_DOUBLE_EQ(v.AsDouble(), static_cast<double>(1.5f));
}

TEST(ScriptValueTest, MoveSemantics) {
  ScriptValue a(std::string("move me"));
  ScriptValue b(std::move(a));
  EXPECT_TRUE(b.IsString());
  EXPECT_EQ(b.AsString(), "move me");
}

TEST(ScriptValueTest, CopySemantics) {
  ScriptValue a(int64_t{99});
  ScriptValue b = a;
  EXPECT_TRUE(b.IsInt());
  EXPECT_EQ(b.AsInt(), 99);
}

TEST(ScriptValueTest, NegativeInt) {
  ScriptValue v(int64_t{-1});
  EXPECT_EQ(v.AsInt(), -1);
}

TEST(ScriptValueTest, EmptyString) {
  ScriptValue v(std::string(""));
  EXPECT_TRUE(v.IsString());
  EXPECT_TRUE(v.AsString().empty());
}

}  // namespace atlas::test
