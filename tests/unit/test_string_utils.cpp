#include <gtest/gtest.h>
#include "foundation/string_utils.hpp"

using namespace atlas::string_utils;

TEST(StringUtils, SplitCharDelimiter)
{
    auto parts = split("a,b,c", ',');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
}

TEST(StringUtils, SplitEmptyInput)
{
    auto parts = split("", ',');
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], "");
}

TEST(StringUtils, SplitTrailingDelimiter)
{
    auto parts = split("a,", ',');
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "");
}

TEST(StringUtils, Trim)
{
    EXPECT_EQ(trim("  hello  "), "hello");
    EXPECT_EQ(trim("\t\n hi \r\n"), "hi");
    EXPECT_EQ(trim(""), "");
    EXPECT_EQ(trim("   "), "");
}

TEST(StringUtils, TrimLeft)
{
    EXPECT_EQ(trim_left("  hello  "), "hello  ");
    EXPECT_EQ(trim_left(""), "");
}

TEST(StringUtils, TrimRight)
{
    EXPECT_EQ(trim_right("  hello  "), "  hello");
    EXPECT_EQ(trim_right(""), "");
}

TEST(StringUtils, ToLower)
{
    EXPECT_EQ(to_lower("Hello WORLD"), "hello world");
    EXPECT_EQ(to_lower(""), "");
}

TEST(StringUtils, ToUpper)
{
    EXPECT_EQ(to_upper("Hello world"), "HELLO WORLD");
    EXPECT_EQ(to_upper(""), "");
}

TEST(StringUtils, IEquals)
{
    EXPECT_TRUE(iequals("Hello", "hello"));
    EXPECT_TRUE(iequals("ABC", "abc"));
    EXPECT_FALSE(iequals("abc", "abcd"));
    EXPECT_TRUE(iequals("", ""));
}

TEST(StringUtils, Join)
{
    std::vector<std::string_view> parts = {"a", "b", "c"};
    EXPECT_EQ(join(parts, ", "), "a, b, c");

    std::vector<std::string_view> single = {"only"};
    EXPECT_EQ(join(single, ", "), "only");

    std::vector<std::string_view> empty = {};
    EXPECT_EQ(join(empty, ", "), "");
}

TEST(StringUtils, ReplaceAll)
{
    EXPECT_EQ(replace_all("aabbaabb", "aa", "X"), "XbbXbb");
    EXPECT_EQ(replace_all("hello", "xyz", "Q"), "hello");
    EXPECT_EQ(replace_all("", "a", "b"), "");
}

TEST(StringUtils, StartsWith)
{
    static_assert(starts_with("hello world", "hello"));
    static_assert(!starts_with("hello", "hello world"));
    EXPECT_TRUE(starts_with("abc", ""));
    EXPECT_FALSE(starts_with("abc", "xyz"));
}

TEST(StringUtils, EndsWith)
{
    static_assert(ends_with("hello world", "world"));
    static_assert(!ends_with("world", "hello world"));
    EXPECT_TRUE(ends_with("abc", ""));
    EXPECT_FALSE(ends_with("abc", "xyz"));
}

TEST(StringUtils, HashFnv1aConstexpr)
{
    constexpr auto h1 = hash_fnv1a("hello");
    constexpr auto h2 = hash_fnv1a("hello");
    constexpr auto h3 = hash_fnv1a("world");
    static_assert(h1 == h2);
    static_assert(h1 != h3);
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, h3);
}

TEST(StringUtils, HashLiteralOperator)
{
    using namespace atlas::string_utils::literals;
    constexpr auto h = "test"_hash;
    EXPECT_EQ(h, hash_fnv1a("test"));
}
