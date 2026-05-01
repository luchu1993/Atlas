#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "clrscript/base_native_provider.h"
#include "clrscript/clr_bootstrap.h"
#include "clrscript/clr_export.h"
#include "clrscript/clr_hot_reload.h"
#include "clrscript/clr_object_registry.h"
#include "clrscript/clr_script_engine.h"
#include "platform/dynamic_library.h"
#include "platform/platform_config.h"

namespace atlas::test {

namespace {

std::filesystem::path EnvPath(const char* var, std::string_view fallback) {
  if (auto* value = std::getenv(var)) return std::filesystem::path{value};
  return std::filesystem::path{std::string(fallback)};
}

std::filesystem::path AtlasEnginePath() {
#if ATLAS_PLATFORM_WINDOWS
  return EnvPath("ATLAS_ENGINE_RLOC", "atlas_engine.dll");
#else
  return EnvPath("ATLAS_ENGINE_RLOC", "libatlas_engine.so");
#endif
}

std::filesystem::path RuntimeConfigPath() {
  return EnvPath("ATLAS_RUNTIME_CONFIG_RLOC", "runtime/atlas_server.runtimeconfig.json");
}

std::filesystem::path AtlasRuntimeAssemblyPath() {
  return EnvPath("ATLAS_RUNTIME_DLL_RLOC", "Atlas.Runtime.dll");
}

class TestProvider final : public BaseNativeProvider {};

}  // namespace

class HotReloadTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    auto lib_result = DynamicLibrary::Load(AtlasEnginePath());
    ASSERT_TRUE(lib_result.HasValue())
        << "Failed to load atlas_engine: " << lib_result.Error().Message()
        << "\nPath: " << AtlasEnginePath().string();
    s_engine_lib = std::move(*lib_result);

    using SetProviderFn = void (*)(void*);
    auto set_fn = s_engine_lib->GetSymbol<SetProviderFn>("AtlasSetNativeApiProvider");
    ASSERT_TRUE(set_fn.HasValue()) << set_fn.Error().Message();
    (*set_fn)(&s_provider);

    using GetFnPtr = void* (*)();
    auto get_set = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorSetFn");
    auto get_clear = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorClearFn");
    auto get_code = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorGetCodeFn");
    ASSERT_TRUE(get_set.HasValue() && get_clear.HasValue() && get_code.HasValue());

    ClrBootstrapArgs args;
    args.error_set =
        reinterpret_cast<decltype(args.error_set)>((*get_set)());
    args.error_clear =
        reinterpret_cast<decltype(args.error_clear)>((*get_clear)());
    args.error_get_code =
        reinterpret_cast<decltype(args.error_get_code)>((*get_code)());

    s_engine = std::make_unique<ClrScriptEngine>();

    ClrScriptEngine::Config cfg;
    cfg.runtime_config_path = RuntimeConfigPath();
    cfg.runtime_assembly_path = AtlasRuntimeAssemblyPath();
    cfg.bootstrap_args = args;

    auto cfg_r = s_engine->Configure(cfg);
    ASSERT_TRUE(cfg_r.HasValue()) << cfg_r.Error().Message();

    auto init_r = s_engine->Initialize();
    ASSERT_TRUE(init_r.HasValue()) << init_r.Error().Message();
  }

  static void TearDownTestSuite() {
    if (s_engine) {
      s_engine->Finalize();
      s_engine.reset();
    }
    s_engine_lib.reset();
  }

 protected:
  static std::unique_ptr<ClrScriptEngine> s_engine;
  static std::optional<DynamicLibrary> s_engine_lib;
  static TestProvider s_provider;
};

std::unique_ptr<ClrScriptEngine> HotReloadTest::s_engine{};
std::optional<DynamicLibrary> HotReloadTest::s_engine_lib{};
TestProvider HotReloadTest::s_provider{};

TEST_F(HotReloadTest, IsEnabledMirrorsConfig) {
  ClrHotReload hr{*s_engine};
  EXPECT_FALSE(hr.IsEnabled());

  ClrHotReload::Config cfg;
  cfg.enabled = false;
  ASSERT_TRUE(hr.Configure(cfg).HasValue());
  EXPECT_FALSE(hr.IsEnabled());

  cfg.enabled = true;
  cfg.script_project_path = std::filesystem::temp_directory_path();
  cfg.output_directory = std::filesystem::temp_directory_path();
  ASSERT_TRUE(hr.Configure(cfg).HasValue());
  EXPECT_TRUE(hr.IsEnabled());
}

TEST_F(HotReloadTest, DisabledPollIsNoOp) {
  ClrHotReload hr{*s_engine};
  ClrHotReload::Config cfg;
  cfg.enabled = false;
  ASSERT_TRUE(hr.Configure(cfg).HasValue());

  for (int i = 0; i < 5; ++i) hr.Poll();
  auto pending = hr.ProcessPending();
  EXPECT_TRUE(pending.HasValue());
}

TEST_F(HotReloadTest, PollWithoutChangesProducesNoReload) {
  auto tmp = std::filesystem::temp_directory_path() / "atlas_hot_reload_test_empty";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);

  ClrHotReload hr{*s_engine};
  ClrHotReload::Config cfg;
  cfg.enabled = true;
  cfg.script_project_path = tmp;
  cfg.output_directory = tmp;
  ASSERT_TRUE(hr.Configure(cfg).HasValue());

  for (int i = 0; i < 5; ++i) hr.Poll();
  auto pending = hr.ProcessPending();
  EXPECT_TRUE(pending.HasValue()) << pending.Error().Message();

  std::filesystem::remove_all(tmp);
}

TEST_F(HotReloadTest, ReloadFailsWhenAssemblyMissing) {
  auto tmp = std::filesystem::temp_directory_path() / "atlas_hot_reload_test_missing";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);

  ClrHotReload hr{*s_engine};
  ClrHotReload::Config cfg;
  cfg.enabled = true;
  cfg.auto_compile = false;
  cfg.output_directory = tmp;
  cfg.assembly_name = "atlas_hot_reload_does_not_exist.dll";
  ASSERT_TRUE(hr.Configure(cfg).HasValue());

  auto r = hr.Reload();
  EXPECT_FALSE(r.HasValue()) << "Reload should have failed (missing assembly)";
  EXPECT_EQ(ClrObjectRegistry::Instance().ActiveCount(), 0u)
      << "Failed reload must not leak ClrObject registrations";

  std::filesystem::remove_all(tmp);
}

TEST_F(HotReloadTest, ReloadFailsWhenCompileFails) {
  ClrHotReload hr{*s_engine};
  ClrHotReload::Config cfg;
  cfg.enabled = true;
  cfg.auto_compile = true;
  cfg.script_project_path = "atlas_hot_reload_definitely_not_a_real_project.csproj";
  cfg.output_directory = std::filesystem::temp_directory_path();
  ASSERT_TRUE(hr.Configure(cfg).HasValue());

  auto r = hr.Reload();
  EXPECT_FALSE(r.HasValue()) << "Reload should have failed (compile target absent)";
  EXPECT_EQ(ClrObjectRegistry::Instance().ActiveCount(), 0u)
      << "Failed compile must not leak ClrObject registrations";
}

}  // namespace atlas::test
