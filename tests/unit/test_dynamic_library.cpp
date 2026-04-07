#include "platform/dynamic_library.hpp"
#include "platform/platform_config.hpp"

#include <gtest/gtest.h>

using namespace atlas;

// ============================================================================
// Load a non-existent library — must return Error, not crash
// ============================================================================

TEST(DynamicLibrary, LoadNonExistentReturnsError)
{
    auto result = DynamicLibrary::load("/no/such/library_xyz_atlas.so");
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message().empty());
}

// ============================================================================
// Platform-specific: load a known system library
// ============================================================================

#if ATLAS_PLATFORM_WINDOWS

// On Windows, kernel32.dll is always present and exports well-known symbols.
static const char* kSystemLib = "kernel32.dll";
static const char* kKnownSymbol = "GetCurrentProcess";

TEST(DynamicLibrary, LoadSystemLibrary)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE(result->is_loaded());
}

TEST(DynamicLibrary, GetKnownSymbol)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());

    using FnPtr = void* (*)();
    auto sym = result->get_symbol<FnPtr>(kKnownSymbol);
    ASSERT_TRUE(sym.has_value()) << sym.error().message();
    EXPECT_NE(*sym, nullptr);
}

TEST(DynamicLibrary, GetUnknownSymbolReturnsError)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());

    using FnPtr = void (*)();
    auto sym = result->get_symbol<FnPtr>("this_symbol_does_not_exist_atlas");
    EXPECT_FALSE(sym.has_value());
}

TEST(DynamicLibrary, UnloadThenIsLoadedFalse)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_loaded());

    result->unload();
    EXPECT_FALSE(result->is_loaded());
}

TEST(DynamicLibrary, GetSymbolAfterUnloadReturnsError)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());
    result->unload();

    using FnPtr = void* (*)();
    auto sym = result->get_symbol<FnPtr>(kKnownSymbol);
    EXPECT_FALSE(sym.has_value());
}

#elif ATLAS_PLATFORM_LINUX

// On Linux, libm.so.6 is universally available and exports "sin".
static const char* kSystemLib = "libm.so.6";
static const char* kKnownSymbol = "sin";

TEST(DynamicLibrary, LoadSystemLibrary)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE(result->is_loaded());
}

TEST(DynamicLibrary, GetKnownSymbol)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());

    using SinFn = double (*)(double);
    auto sym = result->get_symbol<SinFn>(kKnownSymbol);
    ASSERT_TRUE(sym.has_value()) << sym.error().message();
    EXPECT_NE(*sym, nullptr);

    // Sanity-check: call the resolved function
    double val = (*sym)(0.0);
    EXPECT_NEAR(val, 0.0, 1e-12);
}

TEST(DynamicLibrary, GetUnknownSymbolReturnsError)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());

    using FnPtr = void (*)();
    auto sym = result->get_symbol<FnPtr>("this_symbol_does_not_exist_atlas");
    EXPECT_FALSE(sym.has_value());
}

TEST(DynamicLibrary, UnloadThenIsLoadedFalse)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_loaded());

    result->unload();
    EXPECT_FALSE(result->is_loaded());
}

#endif  // platform

// ============================================================================
// Move semantics
// ============================================================================

#if ATLAS_PLATFORM_WINDOWS || ATLAS_PLATFORM_LINUX

TEST(DynamicLibrary, MoveConstructor)
{
    auto result = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(result.has_value());

    DynamicLibrary moved(std::move(*result));
    EXPECT_TRUE(moved.is_loaded());
    EXPECT_FALSE(result->is_loaded());  // source should be empty after move
}

TEST(DynamicLibrary, MoveAssignment)
{
    auto r1 = DynamicLibrary::load(kSystemLib);
    auto r2 = DynamicLibrary::load(kSystemLib);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    *r1 = std::move(*r2);
    EXPECT_TRUE(r1->is_loaded());
    EXPECT_FALSE(r2->is_loaded());
}

#endif
