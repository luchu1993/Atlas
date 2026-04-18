#include <cstdlib>
#include <filesystem>
#include <optional>

#include <gtest/gtest.h>

#include "clrscript/base_native_provider.h"
#include "clrscript/clr_bootstrap.h"
#include "clrscript/clr_error.h"
#include "clrscript/clr_export.h"
#include "clrscript/clr_host.h"
#include "clrscript/clr_invoke.h"
#include "platform/dynamic_library.h"
#include "platform/platform_config.h"
#include "test_paths.h"

// ============================================================================
// Path helpers
// ============================================================================

namespace {

std::filesystem::path runtime_config() {
  auto* rloc = std::getenv("ATLAS_RUNTIME_CONFIG_RLOC");
  if (rloc) return atlas::test::ResolvePath(rloc);
  return "runtime/atlas_server.runtimeconfig.json";
}

std::filesystem::path test_dll() {
  auto* rloc = std::getenv("ATLAS_RUNTIME_TEST_DLL_RLOC");
  if (rloc) return atlas::test::ResolvePath(rloc);
  return "csharp/tests/csharp/Atlas.RuntimeTest/Atlas.RuntimeTest.dll";
}

std::filesystem::path atlas_engine_path() {
  auto* rloc = std::getenv("ATLAS_ENGINE_RLOC");
  if (rloc) return atlas::test::ResolvePath(rloc);
#if ATLAS_PLATFORM_WINDOWS
  return "atlas_engine.dll";
#else
  return "libatlas_engine.so";
#endif
}

static constexpr std::string_view kLifecycleType =
    "Atlas.RuntimeTest.LifecycleTestEntryPoint, Atlas.RuntimeTest";

static constexpr std::string_view kCallbackType =
    "Atlas.RuntimeTest.CallbackEntryPoint, Atlas.RuntimeTest";

}  // namespace

// ============================================================================
// ClrScriptEngineTest — full lifecycle integration test
// ============================================================================
//
// Uses the same DLL-TLS workaround pattern as ClrCallbackTest:
//   1. Load atlas_engine.dll and register provider through it.
//   2. Bootstrap through Atlas.RuntimeTest.dll's RunBootstrap forwarder.
//   3. Bind Phase 3 Lifecycle methods through Atlas.RuntimeTest.dll.

namespace atlas::test {

class ClrScriptEngineTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    auto lib = DynamicLibrary::Load(atlas_engine_path());
    ASSERT_TRUE(lib.HasValue()) << "Cannot load atlas_engine: " << lib.Error().Message()
                                << "\nPath: " << atlas_engine_path().string();
    s_engine_lib = std::move(*lib);

    using SetProviderFn = void (*)(void*);
    auto set_fn = s_engine_lib->GetSymbol<SetProviderFn>("AtlasSetNativeApiProvider");
    ASSERT_TRUE(set_fn.HasValue()) << set_fn.Error().Message();
    (*set_fn)(&s_provider);

    using GetFnPtr = void* (*)();
    using ErrorSetFn = void (*)(int32_t, const char*, int32_t);
    using ErrorClearFn = void (*)();
    using ErrorGetCodeFn = int32_t (*)();

    auto get_set = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorSetFn");
    auto get_clear = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorClearFn");
    auto get_code = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorGetCodeFn");
    ASSERT_TRUE(get_set.HasValue() && get_clear.HasValue() && get_code.HasValue());

    ClrBootstrapArgs args;
    args.error_set = reinterpret_cast<ErrorSetFn>((*get_set)());
    args.error_clear = reinterpret_cast<ErrorClearFn>((*get_clear)());
    args.error_get_code = reinterpret_cast<ErrorGetCodeFn>((*get_code)());

    auto init = s_host.Initialize(runtime_config());
    ASSERT_TRUE(init.HasValue()) << "ClrHost init failed: " << init.Error().Message();

    using BootstrapFn = int (*)(void*, void*);
    auto bootstrap_method =
        s_host.GetMethodAs<BootstrapFn>(test_dll(), kCallbackType, "RunBootstrap");
    ASSERT_TRUE(bootstrap_method.HasValue()) << bootstrap_method.Error().Message();

    ClrObjectVTableOut vtable_out{};
    const int rc = (*bootstrap_method)(static_cast<void*>(&args), static_cast<void*>(&vtable_out));
    ASSERT_EQ(rc, 0) << "RunBootstrap returned non-zero";

    ClrObjectVTable vtable{};
    vtable.free_handle = vtable_out.free_handle;
    vtable.get_type_name = vtable_out.get_type_name;
    vtable.is_none = vtable_out.is_none;
    vtable.to_int64 = vtable_out.to_int64;
    vtable.to_double = vtable_out.to_double;
    vtable.to_string = vtable_out.to_string;
    vtable.to_bool = vtable_out.to_bool;
    SetClrObjectVtable(vtable);

    s_bootstrapped = true;
  }

  static void TearDownTestSuite() {
    s_host.Finalize();
    s_engine_lib.reset();
    s_bootstrapped = false;
  }

 protected:
  void SetUp() override {
    ASSERT_TRUE(s_bootstrapped) << "CLR not bootstrapped";
    ClearClrError();
  }

  template <typename Ret, typename... Args>
  [[nodiscard]] auto bind(ClrStaticMethod<Ret, Args...>& method, std::string_view name,
                          std::string_view type = kLifecycleType) -> bool {
    auto result = method.Bind(s_host, test_dll(), type, name);
    if (!result) {
      ADD_FAILURE() << "bind(" << name << ") failed: " << result.Error().Message();
      return false;
    }
    return true;
  }

  /// RAII helper: calls EngineInit on construction, EngineShutdown on destruction.
  struct EngineScope {
    ClrFallibleMethod<> init;
    ClrFallibleMethod<> shutdown;
    ClrScriptEngineTest& test;
    bool ok = false;

    explicit EngineScope(ClrScriptEngineTest& t) : test(t) {
      ok = test.bind(init, "EngineInit") && test.bind(shutdown, "EngineShutdown");
      if (ok) {
        auto r = init.Invoke();
        ok = r.HasValue();
        if (!ok) ADD_FAILURE() << "EngineInit failed: " << r.Error().Message();
      }
    }
    ~EngineScope() {
      if (ok) (void)shutdown.Invoke();
    }
    EngineScope(const EngineScope&) = delete;
    EngineScope& operator=(const EngineScope&) = delete;
  };

  struct TestProvider : BaseNativeProvider {};

  static ClrHost s_host;
  static std::optional<DynamicLibrary> s_engine_lib;
  static bool s_bootstrapped;
  static TestProvider s_provider;
};

ClrHost ClrScriptEngineTest::s_host{};
std::optional<DynamicLibrary> ClrScriptEngineTest::s_engine_lib{};
bool ClrScriptEngineTest::s_bootstrapped = false;
ClrScriptEngineTest::TestProvider ClrScriptEngineTest::s_provider{};

// ============================================================================
// Test 1: Full lifecycle — EngineInit → OnInit → OnTick → OnShutdown → EngineShutdown
// ============================================================================

TEST_F(ClrScriptEngineTest, FullLifecycle) {
  EngineScope scope(*this);
  ASSERT_TRUE(scope.ok);

  ClrFallibleMethod<uint8_t> on_init;
  ClrFallibleMethod<float> on_tick;
  ClrFallibleMethod<> on_shutdown;

  ASSERT_TRUE(bind(on_init, "OnInit"));
  ASSERT_TRUE(bind(on_tick, "OnTick"));
  ASSERT_TRUE(bind(on_shutdown, "OnShutdown"));

  auto r2 = on_init.Invoke(uint8_t{0});
  ASSERT_TRUE(r2.HasValue()) << r2.Error().Message();

  for (int i = 0; i < 10; ++i) {
    auto r = on_tick.Invoke(0.016f);
    ASSERT_TRUE(r.HasValue()) << "on_tick frame " << i << ": " << r.Error().Message();
  }

  auto r4 = on_shutdown.Invoke();
  ASSERT_TRUE(r4.HasValue()) << r4.Error().Message();
}

// ============================================================================
// Test 2: Log.Info through Lifecycle path
// ============================================================================

TEST_F(ClrScriptEngineTest, LogInfoForwarding) {
  ClrStaticMethod<int, const uint8_t*, int> log_info;
  ASSERT_TRUE(bind(log_info, "LogInfoTest"));

  const char msg[] = "lifecycle-log-test";
  auto r =
      log_info.Invoke(reinterpret_cast<const uint8_t*>(msg), static_cast<int>(sizeof(msg) - 1));
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();
  EXPECT_EQ(*r, 0);
}

// ============================================================================
// Test 3: Time.DeltaTime through NativeApi
// ============================================================================

TEST_F(ClrScriptEngineTest, DeltaTimeQuery) {
  ClrStaticMethod<int, float*> get_dt;
  ASSERT_TRUE(bind(get_dt, "GetDeltaTime"));

  float dt = -1.0f;
  auto r = get_dt.Invoke(&dt);
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();
  EXPECT_GE(dt, 0.0f);
}

// ============================================================================
// Test 4: EntityManager.Count is 0 after init (clean state via EngineScope)
// ============================================================================

TEST_F(ClrScriptEngineTest, EntityCountAfterInit) {
  EngineScope scope(*this);
  ASSERT_TRUE(scope.ok);

  ClrStaticMethod<int, int*> get_count;
  ASSERT_TRUE(bind(get_count, "GetEntityCount"));

  int count = -1;
  auto r = get_count.Invoke(&count);
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();
  EXPECT_EQ(count, 0);
}

// ============================================================================
// Phase 6: Boundary tests
// ============================================================================

TEST_F(ClrScriptEngineTest, ReInitAfterShutdown) {
  {
    EngineScope scope1(*this);
    ASSERT_TRUE(scope1.ok);
  }

  {
    EngineScope scope2(*this);
    ASSERT_TRUE(scope2.ok);

    ClrStaticMethod<int, int*> get_count;
    ASSERT_TRUE(bind(get_count, "GetEntityCount"));

    int count = -1;
    auto r = get_count.Invoke(&count);
    ASSERT_TRUE(r.HasValue()) << r.Error().Message();
    EXPECT_EQ(count, 0);
  }
}

}  // namespace atlas::test
