#include <gtest/gtest.h>
#include "foundation/error.hpp"

#include <string>

using namespace atlas;

TEST(ErrorCode, NameRoundTrip)
{
    auto name = error_code_name(ErrorCode::InvalidArgument);
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(name, "InvalidArgument");
}

TEST(Error, ConstructionAndAccess)
{
    Error err(ErrorCode::IoError, "disk full");
    EXPECT_EQ(err.code(), ErrorCode::IoError);
    EXPECT_EQ(err.message(), "disk full");
    EXPECT_TRUE(static_cast<bool>(err));
}

TEST(Error, DefaultIsNone)
{
    Error err;
    EXPECT_EQ(err.code(), ErrorCode::None);
    EXPECT_FALSE(static_cast<bool>(err));
}

TEST(ResultInt, FromValue)
{
    Result<int> r = 42;
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultInt, FromError)
{
    Result<int> r = Error(ErrorCode::NotFound);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

TEST(ResultVoid, Success)
{
    Result<void> r;
    EXPECT_TRUE(r.has_value());
}

TEST(ResultVoid, ErrorCase)
{
    Result<void> r = Error(ErrorCode::Timeout);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::Timeout);
}

TEST(Result, ValueOr)
{
    Result<int> good = 10;
    EXPECT_EQ(good.value_or(99), 10);

    Result<int> bad = Error(ErrorCode::OutOfMemory);
    EXPECT_EQ(bad.value_or(99), 99);
}

TEST(Result, Transform)
{
    Result<int> r = 5;
    auto r2 = r.transform([](int v) { return std::to_string(v); });
    EXPECT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, "5");

    Result<int> err = Error(ErrorCode::InternalError);
    auto r3 = err.transform([](int v) { return std::to_string(v); });
    EXPECT_FALSE(r3.has_value());
}

TEST(Result, AndThen)
{
    auto double_if_positive = [](int v) -> Result<int>
    {
        if (v > 0) return v * 2;
        return Error(ErrorCode::InvalidArgument);
    };

    Result<int> r = 3;
    auto r2 = r.and_then(double_if_positive);
    EXPECT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 6);

    Result<int> neg = -1;
    auto r3 = neg.and_then(double_if_positive);
    EXPECT_FALSE(r3.has_value());
}

static auto checked_function(bool pass) -> Result<int>
{
    ATLAS_CHECK(pass, Error(ErrorCode::InvalidArgument));
    return 42;
}

TEST(Result, AtlasCheckMacro)
{
    auto ok = checked_function(true);
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 42);

    auto fail = checked_function(false);
    EXPECT_FALSE(fail.has_value());
    EXPECT_EQ(fail.error().code(), ErrorCode::InvalidArgument);
}
