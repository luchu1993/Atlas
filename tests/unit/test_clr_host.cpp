#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "clrscript/clr_host.h"

namespace atlas::test {

// ── Helpers ──────────────────────────────────────────────────────────────────

// Path to the generated runtimeconfig.json (written to the CMake binary dir by
// configure_file in AtlasFindPackages.cmake with the detected runtime version).
static std::filesystem::path runtime_config() {
#ifdef ATLAS_BINARY_DIR
  return std::filesystem::path(ATLAS_BINARY_DIR) / "runtime" / "atlas_server.runtimeconfig.json";
#else
  return "runtime/atlas_server.runtimeconfig.json";
#endif
}

// Path to the compiled SmokeTest assembly (placed by 'dotnet build --output').
static std::filesystem::path smoke_assembly() {
#ifdef ATLAS_BINARY_DIR
  return std::filesystem::path(ATLAS_BINARY_DIR) / "csharp" / "tests" / "csharp" /
         "Atlas.SmokeTest" / "Atlas.SmokeTest.dll";
#else
  return "csharp/tests/csharp/Atlas.SmokeTest/Atlas.SmokeTest.dll";
#endif
}

static constexpr std::string_view kType = "Atlas.SmokeTest.EntryPoint, Atlas.SmokeTest";

// ── Test fixture ─────────────────────────────────────────────────────────────

class ClrHostTest : public ::testing::Test {
 protected:
  ClrHost host;

  void SetUp() override {
    auto result = host.Initialize(runtime_config());
    ASSERT_TRUE(result.HasValue()) << "ClrHost init failed: " << result.Error().Message();
  }

  void TearDown() override { host.Finalize(); }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(ClrHostTest, IsInitializedAfterInit) {
  EXPECT_TRUE(host.IsInitialized());
}

TEST_F(ClrHostTest, Ping) {
  auto method = host.GetMethod(smoke_assembly(), kType, "Ping");
  ASSERT_TRUE(method.HasValue()) << method.Error().Message();

  using PingFn = int (*)();
  EXPECT_EQ(reinterpret_cast<PingFn>(*method)(), 42);
}

TEST_F(ClrHostTest, Add) {
  auto method = host.GetMethod(smoke_assembly(), kType, "Add");
  ASSERT_TRUE(method.HasValue()) << method.Error().Message();

  using AddFn = int (*)(int, int);
  auto add = reinterpret_cast<AddFn>(*method);
  EXPECT_EQ(add(17, 25), 42);
  EXPECT_EQ(add(-1, 1), 0);
  EXPECT_EQ(add(0, 0), 0);
  EXPECT_EQ(add(INT_MAX, 0), INT_MAX);
}

TEST_F(ClrHostTest, StringLength) {
  auto method = host.GetMethod(smoke_assembly(), kType, "StringLength");
  ASSERT_TRUE(method.HasValue()) << method.Error().Message();

  using StringLengthFn = int (*)(const uint8_t*, int);
  auto string_length = reinterpret_cast<StringLengthFn>(*method);

  const char* ascii = "hello";
  EXPECT_EQ(string_length(reinterpret_cast<const uint8_t*>(ascii), 5), 5);

  // UTF-8 multi-byte: "日本語" = 3 characters, 9 bytes
  const char* japanese = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
  EXPECT_EQ(string_length(reinterpret_cast<const uint8_t*>(japanese), 9), 3);
}

TEST_F(ClrHostTest, InitializeTwiceReturnsError) {
  auto result = host.Initialize(runtime_config());
  EXPECT_FALSE(result.HasValue());
}

TEST_F(ClrHostTest, GetMethodForNonexistentMethodReturnsError) {
  auto result = host.GetMethod(smoke_assembly(), kType, "NoSuchMethod");
  EXPECT_FALSE(result.HasValue());
}

// ── Tests without fixture (pre-init / post-finalize) ─────────────────────────

TEST(ClrHostLifecycleTest, GetMethodBeforeInitReturnsError) {
  ClrHost uninit;
  auto result = uninit.GetMethod(smoke_assembly(), kType, "Ping");
  EXPECT_FALSE(result.HasValue());
}

TEST(ClrHostLifecycleTest, FinalizeBeforeInitIsNoOp) {
  ClrHost host;
  host.Finalize();  // must not crash
  EXPECT_FALSE(host.IsInitialized());
}

TEST(ClrHostLifecycleTest, FullLifecycle) {
  ClrHost host;
  ASSERT_TRUE(host.Initialize(runtime_config()).HasValue());
  EXPECT_TRUE(host.IsInitialized());

  auto method = host.GetMethod(smoke_assembly(), kType, "Ping");
  ASSERT_TRUE(method.HasValue());

  using PingFn = int (*)();
  EXPECT_EQ(reinterpret_cast<PingFn>(*method)(), 42);

  host.Finalize();
  EXPECT_FALSE(host.IsInitialized());
}

// ============================================================================
// Phase 6: Boundary tests
// ============================================================================

TEST_F(ClrHostTest, ConcurrentGetFunction) {
  auto asm_path = smoke_assembly();

  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([this, asm_path, &success_count]() {
      using PingFn = int (*)();
      auto result = host.GetMethodAs<PingFn>(asm_path, kType, "Ping");
      if (result.HasValue() && (*result)() == 42) success_count.fetch_add(1);
    });
  }

  for (auto& t : threads) t.join();

  EXPECT_EQ(success_count.load(), 8);
}

}  // namespace atlas::test
