#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"
#include "pyscript/py_convert.hpp"

using namespace atlas;
using namespace atlas::py_convert;

class PyConvertTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
        {
            (void)PyInterpreter::initialize();
        }
    }
};

TEST_F(PyConvertTest, BoolRoundTrip)
{
    auto py_true = to_python(true);
    auto py_false = to_python(false);
    ASSERT_TRUE(static_cast<bool>(py_true));
    ASSERT_TRUE(static_cast<bool>(py_false));

    auto cpp_true = from_python<bool>(py_true.get());
    auto cpp_false = from_python<bool>(py_false.get());
    ASSERT_TRUE(cpp_true.has_value());
    ASSERT_TRUE(cpp_false.has_value());
    EXPECT_TRUE(*cpp_true);
    EXPECT_FALSE(*cpp_false);
}

TEST_F(PyConvertTest, Int32RoundTrip)
{
    auto py = to_python(int32_t{-42});
    auto cpp = from_python<int32_t>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_EQ(*cpp, -42);
}

TEST_F(PyConvertTest, Int64RoundTrip)
{
    auto py = to_python(int64_t{9876543210LL});
    auto cpp = from_python<int64_t>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_EQ(*cpp, 9876543210LL);
}

TEST_F(PyConvertTest, Uint32RoundTrip)
{
    auto py = to_python(uint32_t{3000000000u});
    auto cpp = from_python<uint32_t>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_EQ(*cpp, 3000000000u);
}

TEST_F(PyConvertTest, DoubleRoundTrip)
{
    auto py = to_python(3.14159);
    auto cpp = from_python<double>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_NEAR(*cpp, 3.14159, 1e-10);
}

TEST_F(PyConvertTest, FloatRoundTrip)
{
    auto py = to_python(2.718f);
    auto cpp = from_python<float>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_NEAR(*cpp, 2.718f, 1e-5f);
}

TEST_F(PyConvertTest, StringRoundTrip)
{
    auto py = to_python(std::string_view{"hello atlas"});
    auto cpp = from_python<std::string>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_EQ(*cpp, "hello atlas");
}

TEST_F(PyConvertTest, EmptyStringRoundTrip)
{
    auto py = to_python(std::string_view{""});
    auto cpp = from_python<std::string>(py.get());
    ASSERT_TRUE(cpp.has_value());
    EXPECT_EQ(*cpp, "");
}

TEST_F(PyConvertTest, BytesRoundTrip)
{
    std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0xFF}};
    auto py = to_python(data);
    auto cpp = from_python<std::vector<std::byte>>(py.get());
    ASSERT_TRUE(cpp.has_value());
    ASSERT_EQ(cpp->size(), 3u);
    EXPECT_EQ((*cpp)[0], std::byte{0x01});
    EXPECT_EQ((*cpp)[2], std::byte{0xFF});
}

TEST_F(PyConvertTest, TypeMismatchReturnsError)
{
    auto py_str = to_python(std::string_view{"not a number"});
    auto result = from_python<int32_t>(py_str.get());
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::ScriptTypeError);
}

TEST_F(PyConvertTest, NullReturnsError)
{
    auto result = from_python<int32_t>(nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PyConvertTest, IntToDoubleConversion)
{
    auto py_int = to_python(int32_t{42});
    auto cpp_double = from_python<double>(py_int.get());
    ASSERT_TRUE(cpp_double.has_value());
    EXPECT_NEAR(*cpp_double, 42.0, 1e-10);
}
