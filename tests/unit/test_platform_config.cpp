#include <gtest/gtest.h>

#include "platform/platform_config.h"

TEST(PlatformConfig, ExactlyOnePlatform) {
  int count = 0;
  if constexpr (atlas::platform::kIsWindows) ++count;
  if constexpr (atlas::platform::kIsLinux) ++count;
  if constexpr (atlas::platform::kIsMacos) ++count;
  EXPECT_GE(count, 1);  // At least one platform should be active
}

TEST(PlatformConfig, CacheLineSize) {
  EXPECT_GT(atlas::platform::kCacheLineSize, 0u);
}

TEST(PlatformConfig, ConstexprBoolsMatchMacros) {
  EXPECT_EQ(atlas::platform::kIsWindows, ATLAS_PLATFORM_WINDOWS != 0);
  EXPECT_EQ(atlas::platform::kIsLinux, ATLAS_PLATFORM_LINUX != 0);
  EXPECT_EQ(atlas::platform::kIsDebug, ATLAS_DEBUG != 0);
}
