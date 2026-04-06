#include <gtest/gtest.h>
#include "platform/filesystem.hpp"

#include <filesystem>
#include <string>

namespace fs = atlas::fs;

TEST(Filesystem, ReadWriteTextFileRoundTrip)
{
    auto tmp = fs::temp_directory() / "atlas_test_rw.txt";
    std::string content = "Hello, Atlas!\nLine 2\n";

    auto wr = fs::write_text_file(tmp, content);
    (void)wr;
    ASSERT_TRUE(wr.has_value());

    auto rd = fs::read_text_file(tmp);
    ASSERT_TRUE(rd.has_value());
    EXPECT_EQ(*rd, content);

    (void)fs::remove_file(tmp);
}

TEST(Filesystem, ExistsAfterWrite)
{
    auto tmp = fs::temp_directory() / "atlas_test_exists.txt";
    (void)fs::write_text_file(tmp, "data");
    EXPECT_TRUE(fs::exists(tmp));
    (void)fs::remove_file(tmp);
    EXPECT_FALSE(fs::exists(tmp));
}

TEST(Filesystem, FileSizeCorrect)
{
    auto tmp = fs::temp_directory() / "atlas_test_size.txt";
    std::string data = "12345";
    (void)fs::write_text_file(tmp, data);

    auto sz = fs::file_size(tmp);
    ASSERT_TRUE(sz.has_value());
    EXPECT_EQ(*sz, static_cast<std::uintmax_t>(data.size()));

    (void)fs::remove_file(tmp);
}

TEST(Filesystem, ReadNonexistentReturnsError)
{
    auto result = fs::read_file(fs::temp_directory() / "atlas_nonexistent_file_42.bin");
    EXPECT_FALSE(result.has_value());
}

TEST(Filesystem, CreateDirectoriesNested)
{
    auto dir = fs::temp_directory() / "atlas_test_dirs" / "a" / "b" / "c";
    auto r = fs::create_directories(dir);
    (void)r;
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(fs::exists(dir));

    // Clean up
    std::filesystem::remove_all(fs::temp_directory() / "atlas_test_dirs");
}

TEST(Filesystem, ExecutablePathNonEmpty)
{
    auto ep = fs::executable_path();
    ASSERT_TRUE(ep.has_value());
    EXPECT_FALSE(ep->empty());
}
