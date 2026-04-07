#include "clrscript/clr_marshal.hpp"
#include "math/quaternion.hpp"
#include "math/vector3.hpp"
#include "script/script_value.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace atlas;
using namespace atlas::clr_marshal;

// ============================================================================
// Layout assertions (compile-time; if these fire the test won't link)
// ============================================================================

// Covered by static_assert in clr_marshal.hpp — this test just confirms the
// header compiled successfully with the expected sizes.
TEST(ClrMarshalLayout, StructSizesAreCorrect)
{
#if defined(_WIN64) || defined(__LP64__)
    EXPECT_EQ(sizeof(ClrStringRef), 16u);
    EXPECT_EQ(sizeof(ClrSpanRef), 16u);
    EXPECT_EQ(sizeof(ClrVector3), 12u);
    EXPECT_EQ(sizeof(ClrQuaternion), 16u);
    EXPECT_EQ(sizeof(ClrScriptValue), 24u);
#else
    GTEST_SKIP() << "Layout sizes only verified on 64-bit targets";
#endif
}

TEST(ClrMarshalLayout, BoolSizeIsOne)
{
    EXPECT_EQ(sizeof(bool), 1u);
    EXPECT_EQ(sizeof(uint8_t), 1u);
}

TEST(ClrMarshalLayout, ScriptValueFieldOffsets)
{
#if defined(_WIN64) || defined(__LP64__)
    EXPECT_EQ(offsetof(ClrScriptValue, type), 0u);
    EXPECT_EQ(offsetof(ClrScriptValue, _pad), 4u);
    EXPECT_EQ(offsetof(ClrScriptValue, bool_val), 8u);
    EXPECT_EQ(offsetof(ClrScriptValue, int_val), 8u);
    EXPECT_EQ(offsetof(ClrScriptValue, double_val), 8u);
    EXPECT_EQ(offsetof(ClrScriptValue, string_val), 8u);
    EXPECT_EQ(offsetof(ClrScriptValue, bytes_val), 8u);
    EXPECT_EQ(offsetof(ClrScriptValue, object_val), 8u);
#else
    GTEST_SKIP() << "Offset checks only verified on 64-bit targets";
#endif
}

// ============================================================================
// ClrStringRef — to_string_ref / from_string_ref
// ============================================================================

TEST(ClrStringRef, EmptyStringView)
{
    auto ref = to_string_ref("");
    EXPECT_EQ(ref.length, 0);
    // data may be non-null (points to empty string literal) but length is 0
}

TEST(ClrStringRef, AsciiRoundtrip)
{
    std::string original = "hello world";
    auto ref = to_string_ref(original);

    EXPECT_EQ(ref.length, static_cast<int32_t>(original.size()));
    EXPECT_EQ(ref.data, original.data());  // zero-copy: same pointer

    auto recovered = from_string_ref(ref);
    EXPECT_EQ(recovered, original);
}

TEST(ClrStringRef, UnicodeRoundtrip)
{
    // "Atlas引擎" in UTF-8 (引=E5 BC 95, 擎=E6 93 8E)
    std::string original = "Atlas\xe5\xbc\x95\xe6\x93\x8e";
    auto ref = to_string_ref(original);

    EXPECT_EQ(ref.length, static_cast<int32_t>(original.size()));

    auto recovered = from_string_ref(ref);
    EXPECT_EQ(recovered, original);
}

TEST(ClrStringRef, NullRefReturnsEmptyString)
{
    ClrStringRef null_ref{nullptr, 0};
    auto s = from_string_ref(null_ref);
    EXPECT_TRUE(s.empty());
}

TEST(ClrStringRef, NegativeLengthReturnsEmptyString)
{
    const char* data = "abc";
    ClrStringRef bad{data, -1};
    auto s = from_string_ref(bad);
    EXPECT_TRUE(s.empty());
}

TEST(ClrStringRef, LargeString)
{
    std::string original(10'000, 'x');
    auto ref = to_string_ref(original);
    auto recovered = from_string_ref(ref);
    EXPECT_EQ(recovered, original);
}

// ============================================================================
// ClrSpanRef — to_span_ref
// ============================================================================

TEST(ClrSpanRef, EmptySpan)
{
    auto ref = to_span_ref({});
    EXPECT_EQ(ref.length, 0);
}

TEST(ClrSpanRef, NonEmptySpanPreservesPointerAndLength)
{
    std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0xFF}};
    std::span<const std::byte> span{data};

    auto ref = to_span_ref(span);

    EXPECT_EQ(ref.data, data.data());
    EXPECT_EQ(ref.length, 3);
}

// ============================================================================
// Vector3 — to_vector3 / from_vector3
// ============================================================================

TEST(ClrVector3, Roundtrip)
{
    math::Vector3 v{1.5f, -2.25f, 3.125f};
    auto cv = to_vector3(v);
    auto recovered = from_vector3(cv);

    EXPECT_FLOAT_EQ(cv.x, 1.5f);
    EXPECT_FLOAT_EQ(cv.y, -2.25f);
    EXPECT_FLOAT_EQ(cv.z, 3.125f);

    EXPECT_FLOAT_EQ(recovered.x, v.x);
    EXPECT_FLOAT_EQ(recovered.y, v.y);
    EXPECT_FLOAT_EQ(recovered.z, v.z);
}

TEST(ClrVector3, ZeroVector)
{
    auto cv = to_vector3(math::Vector3::zero());
    EXPECT_FLOAT_EQ(cv.x, 0.0f);
    EXPECT_FLOAT_EQ(cv.y, 0.0f);
    EXPECT_FLOAT_EQ(cv.z, 0.0f);
}

// ============================================================================
// Quaternion — to_quaternion / from_quaternion
// ============================================================================

TEST(ClrQuaternion, IdentityRoundtrip)
{
    auto q = math::Quaternion::identity();
    auto cq = to_quaternion(q);
    auto rec = from_quaternion(cq);

    EXPECT_FLOAT_EQ(cq.x, 0.0f);
    EXPECT_FLOAT_EQ(cq.y, 0.0f);
    EXPECT_FLOAT_EQ(cq.z, 0.0f);
    EXPECT_FLOAT_EQ(cq.w, 1.0f);

    EXPECT_FLOAT_EQ(rec.x, q.x);
    EXPECT_FLOAT_EQ(rec.y, q.y);
    EXPECT_FLOAT_EQ(rec.z, q.z);
    EXPECT_FLOAT_EQ(rec.w, q.w);
}

TEST(ClrQuaternion, ArbitraryValues)
{
    math::Quaternion q{0.1f, 0.2f, 0.3f, 0.4f};
    auto cq = to_quaternion(q);
    auto rec = from_quaternion(cq);

    EXPECT_FLOAT_EQ(rec.x, q.x);
    EXPECT_FLOAT_EQ(rec.y, q.y);
    EXPECT_FLOAT_EQ(rec.z, q.z);
    EXPECT_FLOAT_EQ(rec.w, q.w);
}

// ============================================================================
// ScriptValue — to_script_value
// ============================================================================

TEST(ClrScriptValue, NoneType)
{
    ScriptValue sv{};
    auto cv = to_script_value(sv);
    EXPECT_EQ(cv.type, ClrScriptValueType::None);
}

TEST(ClrScriptValue, BoolTrue)
{
    auto cv = to_script_value(ScriptValue{true});
    EXPECT_EQ(cv.type, ClrScriptValueType::Bool);
    EXPECT_EQ(cv.bool_val, uint8_t{1});
}

TEST(ClrScriptValue, BoolFalse)
{
    auto cv = to_script_value(ScriptValue{false});
    EXPECT_EQ(cv.type, ClrScriptValueType::Bool);
    EXPECT_EQ(cv.bool_val, uint8_t{0});
}

TEST(ClrScriptValue, Int64Zero)
{
    auto cv = to_script_value(ScriptValue{int64_t{0}});
    EXPECT_EQ(cv.type, ClrScriptValueType::Int64);
    EXPECT_EQ(cv.int_val, int64_t{0});
}

TEST(ClrScriptValue, Int64Max)
{
    auto val = std::numeric_limits<int64_t>::max();
    auto cv = to_script_value(ScriptValue{val});
    EXPECT_EQ(cv.type, ClrScriptValueType::Int64);
    EXPECT_EQ(cv.int_val, val);
}

TEST(ClrScriptValue, Int64Min)
{
    auto val = std::numeric_limits<int64_t>::min();
    auto cv = to_script_value(ScriptValue{val});
    EXPECT_EQ(cv.type, ClrScriptValueType::Int64);
    EXPECT_EQ(cv.int_val, val);
}

TEST(ClrScriptValue, Double)
{
    auto cv = to_script_value(ScriptValue{3.14});
    EXPECT_EQ(cv.type, ClrScriptValueType::Double);
    EXPECT_DOUBLE_EQ(cv.double_val, 3.14);
}

TEST(ClrScriptValue, DoubleNaN)
{
    auto cv = to_script_value(ScriptValue{std::numeric_limits<double>::quiet_NaN()});
    EXPECT_EQ(cv.type, ClrScriptValueType::Double);
    EXPECT_TRUE(std::isnan(cv.double_val));
}

TEST(ClrScriptValue, DoubleInfinity)
{
    auto cv = to_script_value(ScriptValue{std::numeric_limits<double>::infinity()});
    EXPECT_EQ(cv.type, ClrScriptValueType::Double);
    EXPECT_TRUE(std::isinf(cv.double_val));
}

TEST(ClrScriptValue, StringZeroCopy)
{
    ScriptValue sv{std::string{"hello"}};
    auto cv = to_script_value(sv);

    EXPECT_EQ(cv.type, ClrScriptValueType::String);
    EXPECT_EQ(cv.string_val.length, 5);
    // Zero-copy: pointer must be into sv's internal buffer
    EXPECT_EQ(cv.string_val.data, sv.as_string().data());
}

TEST(ClrScriptValue, StringUnicode)
{
    std::string utf8 = "Atlas\xe5\xbc\x95\xe6\x93\x8e";
    ScriptValue sv{utf8};
    auto cv = to_script_value(sv);

    EXPECT_EQ(cv.type, ClrScriptValueType::String);
    EXPECT_EQ(cv.string_val.length, static_cast<int32_t>(utf8.size()));
}

TEST(ClrScriptValue, EmptyString)
{
    ScriptValue sv{std::string{}};
    auto cv = to_script_value(sv);

    EXPECT_EQ(cv.type, ClrScriptValueType::String);
    EXPECT_EQ(cv.string_val.length, 0);
}

TEST(ClrScriptValue, BytesZeroCopy)
{
    ScriptValue::Bytes data = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    ScriptValue sv{data};
    auto cv = to_script_value(sv);

    EXPECT_EQ(cv.type, ClrScriptValueType::Bytes);
    EXPECT_EQ(cv.bytes_val.length, 4);
    EXPECT_EQ(cv.bytes_val.data, sv.as_bytes().data());
}

TEST(ClrScriptValue, EmptyBytes)
{
    ScriptValue sv{ScriptValue::Bytes{}};
    auto cv = to_script_value(sv);
    EXPECT_EQ(cv.type, ClrScriptValueType::Bytes);
    EXPECT_EQ(cv.bytes_val.length, 0);
}

// ============================================================================
// ScriptValue — from_script_value (round-trips)
// ============================================================================

TEST(ClrScriptValueRoundtrip, None)
{
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::None;
    auto sv = from_script_value(cv);
    EXPECT_TRUE(sv.is_none());
}

TEST(ClrScriptValueRoundtrip, BoolTrue)
{
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::Bool;
    cv.bool_val = 1;
    auto sv = from_script_value(cv);
    ASSERT_TRUE(sv.is_bool());
    EXPECT_TRUE(sv.as_bool());
}

TEST(ClrScriptValueRoundtrip, BoolFalse)
{
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::Bool;
    cv.bool_val = 0;
    auto sv = from_script_value(cv);
    ASSERT_TRUE(sv.is_bool());
    EXPECT_FALSE(sv.as_bool());
}

TEST(ClrScriptValueRoundtrip, Int64)
{
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::Int64;
    cv.int_val = -42LL;
    auto sv = from_script_value(cv);
    ASSERT_TRUE(sv.is_int());
    EXPECT_EQ(sv.as_int(), -42LL);
}

TEST(ClrScriptValueRoundtrip, Double)
{
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::Double;
    cv.double_val = 2.71828;
    auto sv = from_script_value(cv);
    ASSERT_TRUE(sv.is_double());
    EXPECT_DOUBLE_EQ(sv.as_double(), 2.71828);
}

TEST(ClrScriptValueRoundtrip, StringCopiesData)
{
    std::string original = "roundtrip test";
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::String;
    cv.string_val.data = original.data();
    cv.string_val.length = static_cast<int32_t>(original.size());

    auto sv = from_script_value(cv);
    ASSERT_TRUE(sv.is_string());
    EXPECT_EQ(sv.as_string(), original);
    // from_script_value must copy, not alias the original pointer
    EXPECT_NE(sv.as_string().data(), original.data());
}

TEST(ClrScriptValueRoundtrip, BytesCopiesData)
{
    std::vector<std::byte> original = {std::byte{1}, std::byte{2}, std::byte{3}};
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::Bytes;
    cv.bytes_val.data = original.data();
    cv.bytes_val.length = static_cast<int32_t>(original.size());

    auto sv = from_script_value(cv);
    ASSERT_TRUE(sv.is_bytes());
    EXPECT_EQ(sv.as_bytes(), original);
    // from_script_value must copy
    EXPECT_NE(sv.as_bytes().data(), original.data());
}

TEST(ClrScriptValueRoundtrip, ObjectReturnsNoneUntilPhase22)
{
    // Phase 2.2 will implement proper GCHandle reconstruction.
    // Until then, Object → from_script_value returns None.
    ClrScriptValue cv{};
    cv.type = ClrScriptValueType::Object;
    cv.object_val = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEFu));
    auto sv = from_script_value(cv);
    EXPECT_TRUE(sv.is_none());
}

TEST(ClrScriptValueRoundtrip, FullScriptValueToClrAndBack)
{
    // End-to-end: ScriptValue → ClrScriptValue → ScriptValue
    ScriptValue original{std::string{"end-to-end"}};
    auto cv = to_script_value(original);
    auto recovered = from_script_value(cv);

    ASSERT_TRUE(recovered.is_string());
    EXPECT_EQ(recovered.as_string(), "end-to-end");
}
