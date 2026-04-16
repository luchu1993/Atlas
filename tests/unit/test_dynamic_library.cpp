#include <gtest/gtest.h>

#include "platform/dynamic_library.h"
#include "platform/platform_config.h"

using namespace atlas;

// ============================================================================
// Load a non-existent library — must return Error, not crash
// ============================================================================

TEST(DynamicLibrary, LoadNonExistentReturnsError) {
  auto result = DynamicLibrary::Load("/no/such/library_xyz_atlas.so");
  EXPECT_FALSE(result.HasValue());
  EXPECT_FALSE(result.Error().Message().empty());
}

// ============================================================================
// Platform-specific: load a known system library
// ============================================================================

#if ATLAS_PLATFORM_WINDOWS

// On Windows, kernel32.dll is always present and exports well-known symbols.
static const char* kSystemLib = "kernel32.dll";
static const char* kKnownSymbol = "GetCurrentProcess";

TEST(DynamicLibrary, LoadSystemLibrary) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_TRUE(result->IsLoaded());
}

TEST(DynamicLibrary, GetKnownSymbol) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());

  using FnPtr = void* (*)();
  auto sym = result->GetSymbol<FnPtr>(kKnownSymbol);
  ASSERT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST(DynamicLibrary, GetUnknownSymbolReturnsError) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());

  using FnPtr = void (*)();
  auto sym = result->GetSymbol<FnPtr>("this_symbol_does_not_exist_atlas");
  EXPECT_FALSE(sym.HasValue());
}

TEST(DynamicLibrary, UnloadThenIsLoadedFalse) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->IsLoaded());

  result->Unload();
  EXPECT_FALSE(result->IsLoaded());
}

TEST(DynamicLibrary, GetSymbolAfterUnloadReturnsError) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());
  result->Unload();

  using FnPtr = void* (*)();
  auto sym = result->GetSymbol<FnPtr>(kKnownSymbol);
  EXPECT_FALSE(sym.HasValue());
}

#elif ATLAS_PLATFORM_LINUX

// On Linux, libm.so.6 is universally available and exports "sin".
static const char* kSystemLib = "libm.so.6";
static const char* kKnownSymbol = "sin";

TEST(DynamicLibrary, LoadSystemLibrary) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_TRUE(result->IsLoaded());
}

TEST(DynamicLibrary, GetKnownSymbol) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());

  using SinFn = double (*)(double);
  auto sym = result->GetSymbol<SinFn>(kKnownSymbol);
  ASSERT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);

  // Sanity-check: call the resolved function
  double val = (*sym)(0.0);
  EXPECT_NEAR(val, 0.0, 1e-12);
}

TEST(DynamicLibrary, GetUnknownSymbolReturnsError) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());

  using FnPtr = void (*)();
  auto sym = result->GetSymbol<FnPtr>("this_symbol_does_not_exist_atlas");
  EXPECT_FALSE(sym.HasValue());
}

TEST(DynamicLibrary, UnloadThenIsLoadedFalse) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->IsLoaded());

  result->Unload();
  EXPECT_FALSE(result->IsLoaded());
}

#endif  // platform

// ============================================================================
// Move semantics
// ============================================================================

#if ATLAS_PLATFORM_WINDOWS || ATLAS_PLATFORM_LINUX

TEST(DynamicLibrary, MoveConstructor) {
  auto result = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(result.HasValue());

  DynamicLibrary moved(std::move(*result));
  EXPECT_TRUE(moved.IsLoaded());
  EXPECT_FALSE(result->IsLoaded());  // source should be empty after move
}

TEST(DynamicLibrary, MoveAssignment) {
  auto r1 = DynamicLibrary::Load(kSystemLib);
  auto r2 = DynamicLibrary::Load(kSystemLib);
  ASSERT_TRUE(r1.HasValue());
  ASSERT_TRUE(r2.HasValue());

  *r1 = std::move(*r2);
  EXPECT_TRUE(r1->IsLoaded());
  EXPECT_FALSE(r2->IsLoaded());
}

#endif
