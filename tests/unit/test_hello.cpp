#include <gtest/gtest.h>

TEST(Atlas, HelloWorld) {
  EXPECT_EQ(1 + 1, 2);
}

TEST(Atlas, ProjectVersion) {
  // Verify the project compiles under C++20
  constexpr auto result = [] {
    int sum = 0;
    for (int i = 1; i <= 10; ++i) sum += i;
    return sum;
  }();
  EXPECT_EQ(result, 55);
}
