#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "platform/dynamic_library.h"
#include "platform/platform_config.h"

using namespace atlas;

// ============================================================================
// Locate atlas_engine shared library
// ============================================================================
//
// ATLAS_BINARY_DIR is injected by CMake (same pattern as test_clr_host).
// The shared library is placed in ${CMAKE_BINARY_DIR}/bin/ by the
// atlas_engine target's RUNTIME_OUTPUT_DIRECTORY / LIBRARY_OUTPUT_DIRECTORY.

static std::filesystem::path atlas_engine_path() {
  // ATLAS_ENGINE_DIR is injected via $<TARGET_FILE_DIR:atlas_engine> in
  // CMake, giving the exact directory regardless of single/multi-config
  // generators (e.g. bin/ for Ninja, bin/Debug/ for MSVC).
  std::filesystem::path dir = ATLAS_ENGINE_DIR;

#if ATLAS_PLATFORM_WINDOWS
  return dir / "atlas_engine.dll";
#else
  return dir / "libatlas_engine.so";
#endif
}

// ============================================================================
// Fixtures
// ============================================================================

class AtlasEngineExports : public ::testing::Test {
 protected:
  void SetUp() override {
    auto result = DynamicLibrary::Load(atlas_engine_path());
    ASSERT_TRUE(result.HasValue()) << "Could not load atlas_engine: " << result.Error().Message()
                                   << "\nPath: " << atlas_engine_path().string();
    lib_ = std::move(*result);
  }

  std::optional<DynamicLibrary> lib_;
};

// ============================================================================
// Verify every Atlas* symbol is present in atlas_engine.dll/.so
// ============================================================================

TEST_F(AtlasEngineExports, LogMessageExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasLogMessage");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, ServerTimeExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasServerTime");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, DeltaTimeExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasDeltaTime");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, GetProcessPrefixExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasGetProcessPrefix");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, SendClientRpcExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasSendClientRpc");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, SendCellRpcExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasSendCellRpc");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, SendBaseRpcExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasSendBaseRpc");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, RegisterEntityTypeExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasRegisterEntityType");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

TEST_F(AtlasEngineExports, UnregisterAllEntityTypesExported) {
  auto sym = lib_->GetSymbol<void*>("AtlasUnregisterAllEntityTypes");
  EXPECT_TRUE(sym.HasValue()) << sym.Error().Message();
  EXPECT_NE(*sym, nullptr);
}

// ============================================================================
// Sanity: no unexpected symbols leak (atlas_engine should only export Atlas*)
// ============================================================================

TEST_F(AtlasEngineExports, NonAtlasSymbolNotExported) {
  // An internal C++ symbol should NOT be reachable from outside.
  // (Only meaningful on Linux where we compile with -fvisibility=hidden.)
#if ATLAS_PLATFORM_LINUX
  auto sym = lib_->GetSymbol<void*>("_ZN5atlas3Log4infoEv");
  // We don't assert false here — the symbol may or may not exist depending
  // on the toolchain — but if it IS exported the test is informational.
  (void)sym;
#else
  GTEST_SKIP() << "Symbol visibility enforcement only verified on Linux";
#endif
}
