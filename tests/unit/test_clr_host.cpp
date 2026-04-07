#include "clrscript/clr_host.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace atlas::test
{

// ── Helpers ──────────────────────────────────────────────────────────────────

// Path to the runtimeconfig.json (relative to CTest working directory, which
// is the CMake binary directory).
static std::filesystem::path runtime_config()
{
    // The runtimeconfig is copied / referenced from the source tree.
    // CMake sets the working directory for ctest to the build root, so we go
    // up to the source root via CMAKE_SOURCE_DIR — injected as a compile def.
#ifdef ATLAS_SOURCE_DIR
    return std::filesystem::path(ATLAS_SOURCE_DIR) / "runtime" / "atlas_server.runtimeconfig.json";
#else
    return "runtime/atlas_server.runtimeconfig.json";
#endif
}

// Path to the compiled SmokeTest assembly (placed by 'dotnet build --output').
static std::filesystem::path smoke_assembly()
{
#ifdef ATLAS_BINARY_DIR
    return std::filesystem::path(ATLAS_BINARY_DIR) / "csharp" / "tests" / "csharp" /
           "Atlas.SmokeTest" / "Atlas.SmokeTest.dll";
#else
    return "csharp/tests/csharp/Atlas.SmokeTest/Atlas.SmokeTest.dll";
#endif
}

static constexpr std::string_view kType = "Atlas.SmokeTest.EntryPoint, Atlas.SmokeTest";

// ── Test fixture ─────────────────────────────────────────────────────────────

class ClrHostTest : public ::testing::Test
{
protected:
    ClrHost host;

    void SetUp() override
    {
        auto result = host.initialize(runtime_config());
        ASSERT_TRUE(result.has_value()) << "ClrHost init failed: " << result.error().message();
    }

    void TearDown() override { host.finalize(); }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(ClrHostTest, IsInitializedAfterInit)
{
    EXPECT_TRUE(host.is_initialized());
}

TEST_F(ClrHostTest, Ping)
{
    auto method = host.get_method(smoke_assembly(), kType, "Ping");
    ASSERT_TRUE(method.has_value()) << method.error().message();

    using PingFn = int (*)();
    EXPECT_EQ(reinterpret_cast<PingFn>(*method)(), 42);
}

TEST_F(ClrHostTest, Add)
{
    auto method = host.get_method(smoke_assembly(), kType, "Add");
    ASSERT_TRUE(method.has_value()) << method.error().message();

    using AddFn = int (*)(int, int);
    auto add = reinterpret_cast<AddFn>(*method);
    EXPECT_EQ(add(17, 25), 42);
    EXPECT_EQ(add(-1, 1), 0);
    EXPECT_EQ(add(0, 0), 0);
    EXPECT_EQ(add(INT_MAX, 0), INT_MAX);
}

TEST_F(ClrHostTest, StringLength)
{
    auto method = host.get_method(smoke_assembly(), kType, "StringLength");
    ASSERT_TRUE(method.has_value()) << method.error().message();

    using StringLengthFn = int (*)(const uint8_t*, int);
    auto string_length = reinterpret_cast<StringLengthFn>(*method);

    const char* ascii = "hello";
    EXPECT_EQ(string_length(reinterpret_cast<const uint8_t*>(ascii), 5), 5);

    // UTF-8 multi-byte: "日本語" = 3 characters, 9 bytes
    const char* japanese = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    EXPECT_EQ(string_length(reinterpret_cast<const uint8_t*>(japanese), 9), 3);
}

TEST_F(ClrHostTest, InitializeTwiceReturnsError)
{
    auto result = host.initialize(runtime_config());
    EXPECT_FALSE(result.has_value());
}

TEST_F(ClrHostTest, GetMethodForNonexistentMethodReturnsError)
{
    auto result = host.get_method(smoke_assembly(), kType, "NoSuchMethod");
    EXPECT_FALSE(result.has_value());
}

// ── Tests without fixture (pre-init / post-finalize) ─────────────────────────

TEST(ClrHostLifecycleTest, GetMethodBeforeInitReturnsError)
{
    ClrHost uninit;
    auto result = uninit.get_method(smoke_assembly(), kType, "Ping");
    EXPECT_FALSE(result.has_value());
}

TEST(ClrHostLifecycleTest, FinalizeBeforeInitIsNoOp)
{
    ClrHost host;
    host.finalize();  // must not crash
    EXPECT_FALSE(host.is_initialized());
}

TEST(ClrHostLifecycleTest, FullLifecycle)
{
    ClrHost host;
    ASSERT_TRUE(host.initialize(runtime_config()).has_value());
    EXPECT_TRUE(host.is_initialized());

    auto method = host.get_method(smoke_assembly(), kType, "Ping");
    ASSERT_TRUE(method.has_value());

    using PingFn = int (*)();
    EXPECT_EQ(reinterpret_cast<PingFn>(*method)(), 42);

    host.finalize();
    EXPECT_FALSE(host.is_initialized());
}

}  // namespace atlas::test
