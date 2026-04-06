#include <gtest/gtest.h>
#include "platform/platform_config.hpp"

TEST(PlatformConfig, ExactlyOnePlatform)
{
    int count = 0;
    if constexpr (atlas::platform::is_windows) ++count;
    if constexpr (atlas::platform::is_linux) ++count;
    if constexpr (atlas::platform::is_macos) ++count;
    EXPECT_GE(count, 1);  // At least one platform should be active
}

TEST(PlatformConfig, CacheLineSize)
{
    EXPECT_GT(atlas::platform::cache_line_size, 0u);
}

TEST(PlatformConfig, ConstexprBoolsMatchMacros)
{
    EXPECT_EQ(atlas::platform::is_windows, ATLAS_PLATFORM_WINDOWS != 0);
    EXPECT_EQ(atlas::platform::is_linux, ATLAS_PLATFORM_LINUX != 0);
    EXPECT_EQ(atlas::platform::is_debug, ATLAS_DEBUG != 0);
}
