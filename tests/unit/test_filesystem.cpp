#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "platform/filesystem.h"

namespace fs = atlas::fs;

TEST(Filesystem, ReadWriteTextFileRoundTrip) {
  auto tmp = fs::TempDirectory() / "atlas_test_rw.txt";
  std::string content = "Hello, Atlas!\nLine 2\n";

  auto wr = fs::WriteTextFile(tmp, content);
  (void)wr;
  ASSERT_TRUE(wr.HasValue());

  auto rd = fs::ReadTextFile(tmp);
  ASSERT_TRUE(rd.HasValue());
  EXPECT_EQ(*rd, content);

  (void)fs::RemoveFile(tmp);
}

TEST(Filesystem, ExistsAfterWrite) {
  auto tmp = fs::TempDirectory() / "atlas_test_exists.txt";
  (void)fs::WriteTextFile(tmp, "data");
  EXPECT_TRUE(fs::Exists(tmp));
  (void)fs::RemoveFile(tmp);
  EXPECT_FALSE(fs::Exists(tmp));
}

TEST(Filesystem, FileSizeCorrect) {
  auto tmp = fs::TempDirectory() / "atlas_test_size.txt";
  std::string data = "12345";
  (void)fs::WriteTextFile(tmp, data);

  auto sz = fs::FileSize(tmp);
  ASSERT_TRUE(sz.HasValue());
  EXPECT_EQ(*sz, static_cast<std::uintmax_t>(data.size()));

  (void)fs::RemoveFile(tmp);
}

TEST(Filesystem, ReadNonexistentReturnsError) {
  auto result = fs::ReadFile(fs::TempDirectory() / "atlas_nonexistent_file_42.bin");
  EXPECT_FALSE(result.HasValue());
}

TEST(Filesystem, CreateDirectoriesNested) {
  auto dir = fs::TempDirectory() / "atlas_test_dirs" / "a" / "b" / "c";
  auto r = fs::CreateDirectories(dir);
  (void)r;
  ASSERT_TRUE(r.HasValue());
  EXPECT_TRUE(fs::Exists(dir));

  // Clean up
  std::filesystem::remove_all(fs::TempDirectory() / "atlas_test_dirs");
}

TEST(Filesystem, ExecutablePathNonEmpty) {
  auto ep = fs::ExecutablePath();
  ASSERT_TRUE(ep.HasValue());
  EXPECT_FALSE(ep->empty());
}
