#include <string>

#include <gtest/gtest.h>

#include "foundation/error.h"

using namespace atlas;

TEST(ErrorCode, NameRoundTrip) {
  auto name = ErrorCodeName(ErrorCode::kInvalidArgument);
  EXPECT_FALSE(name.empty());
  EXPECT_EQ(name, "InvalidArgument");
}

TEST(Error, ConstructionAndAccess) {
  Error err(ErrorCode::kIoError, "disk full");
  EXPECT_EQ(err.Code(), ErrorCode::kIoError);
  EXPECT_EQ(err.Message(), "disk full");
  EXPECT_TRUE(static_cast<bool>(err));
}

TEST(Error, DefaultIsNone) {
  Error err;
  EXPECT_EQ(err.Code(), ErrorCode::kNone);
  EXPECT_FALSE(static_cast<bool>(err));
}

TEST(ResultInt, FromValue) {
  Result<int> r = 42;
  EXPECT_TRUE(r.HasValue());
  EXPECT_EQ(*r, 42);
  EXPECT_EQ(r.Value(), 42);
}

TEST(ResultInt, FromError) {
  Result<int> r = Error(ErrorCode::kNotFound);
  EXPECT_FALSE(r.HasValue());
  EXPECT_EQ(r.Error().Code(), ErrorCode::kNotFound);
}

TEST(ResultVoid, Success) {
  Result<void> r;
  EXPECT_TRUE(r.HasValue());
}

TEST(ResultVoid, ErrorCase) {
  Result<void> r = Error(ErrorCode::kTimeout);
  EXPECT_FALSE(r.HasValue());
  EXPECT_EQ(r.Error().Code(), ErrorCode::kTimeout);
}

TEST(Result, ValueOr) {
  Result<int> good = 10;
  EXPECT_EQ(good.ValueOr(99), 10);

  Result<int> bad = Error(ErrorCode::kOutOfMemory);
  EXPECT_EQ(bad.ValueOr(99), 99);
}

TEST(Result, Transform) {
  Result<int> r = 5;
  auto r2 = r.Transform([](int v) { return std::to_string(v); });
  EXPECT_TRUE(r2.HasValue());
  EXPECT_EQ(*r2, "5");

  Result<int> err = Error(ErrorCode::kInternalError);
  auto r3 = err.Transform([](int v) { return std::to_string(v); });
  EXPECT_FALSE(r3.HasValue());
}

TEST(Result, AndThen) {
  auto double_if_positive = [](int v) -> Result<int> {
    if (v > 0) return v * 2;
    return Error(ErrorCode::kInvalidArgument);
  };

  Result<int> r = 3;
  auto r2 = r.AndThen(double_if_positive);
  EXPECT_TRUE(r2.HasValue());
  EXPECT_EQ(*r2, 6);

  Result<int> neg = -1;
  auto r3 = neg.AndThen(double_if_positive);
  EXPECT_FALSE(r3.HasValue());
}

static auto checked_function(bool pass) -> Result<int> {
  ATLAS_CHECK(pass, Error(ErrorCode::kInvalidArgument));
  return 42;
}

TEST(Result, AtlasCheckMacro) {
  auto ok = checked_function(true);
  EXPECT_TRUE(ok.HasValue());
  EXPECT_EQ(*ok, 42);

  auto fail = checked_function(false);
  EXPECT_FALSE(fail.HasValue());
  EXPECT_EQ(fail.Error().Code(), ErrorCode::kInvalidArgument);
}
