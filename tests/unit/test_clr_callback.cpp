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
#include "clrscript/clr_object.h"
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

static constexpr std::string_view kTestType =
    "Atlas.RuntimeTest.CallbackEntryPoint, Atlas.RuntimeTest";

}  // namespace

// ============================================================================
// Test fixture — CLR initialized ONCE per suite (process-global singleton)
// ============================================================================
//
// CoreCLR can only be loaded once per process.  SetUpTestSuite initializes the
// CLR and bootstraps the error bridge + GCHandle vtable once for all tests.
//
// Key architectural constraint:
//   The test binary statically links atlas_clrscript, while atlas_engine.dll
//   also links it.  This creates TWO copies of all globals (g_provider,
//   t_clr_error).  The fixture resolves this by:
//     1. Registering the provider through the DLL's export (AtlasSetNativeApiProvider).
//     2. Passing the DLL's clr_error_* function pointers to Bootstrap so C# writes
//        into the DLL's TLS — matching what the DLL's AtlasHasClrError() reads.
//     3. Bootstrapping through Atlas.RuntimeTest.dll's RunBootstrap forwarder
//        (not clr_bootstrap) to avoid loading a second Atlas.Runtime.dll Assembly.

namespace atlas::test {

class ClrCallbackTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    // Step 1: Load atlas_engine.dll.
    auto lib = DynamicLibrary::Load(atlas_engine_path());
    ASSERT_TRUE(lib.HasValue()) << "Cannot load atlas_engine: " << lib.Error().Message()
                                << "\nPath: " << atlas_engine_path().string();
    s_engine_lib = std::move(*lib);

    // Step 2: Register INativeApiProvider through the DLL's export.
    using SetProviderFn = void (*)(void*);
    auto set_fn = s_engine_lib->GetSymbol<SetProviderFn>("AtlasSetNativeApiProvider");
    ASSERT_TRUE(set_fn.HasValue()) << set_fn.Error().Message();
    (*set_fn)(&s_provider);

    // Step 3: Obtain the DLL's error-bridge function pointers.
    using GetFnPtr = void* (*)();
    using ErrorSetFn = void (*)(int32_t, const char*, int32_t);
    using ErrorClearFn = void (*)();
    using ErrorGetCodeFn = int32_t (*)();

    auto get_set = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorSetFn");
    auto get_clear = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorClearFn");
    auto get_code = s_engine_lib->GetSymbol<GetFnPtr>("AtlasGetClrErrorGetCodeFn");
    ASSERT_TRUE(get_set.HasValue() && get_clear.HasValue() && get_code.HasValue())
        << "DLL error bridge exports not found";

    ClrBootstrapArgs args;
    args.error_set = reinterpret_cast<ErrorSetFn>((*get_set)());
    args.error_clear = reinterpret_cast<ErrorClearFn>((*get_clear)());
    args.error_get_code = reinterpret_cast<ErrorGetCodeFn>((*get_code)());

    s_dll_has_error = *s_engine_lib->GetSymbol<int32_t (*)()>("AtlasHasClrError");
    s_dll_read_error = *s_engine_lib->GetSymbol<int32_t (*)(char*, int32_t)>("AtlasReadClrError");
    s_dll_clear_error = *s_engine_lib->GetSymbol<void (*)()>("AtlasClearClrError");

    // Step 4: Initialize CLR.
    auto init = s_host.Initialize(runtime_config());
    ASSERT_TRUE(init.HasValue()) << "ClrHost init failed: " << init.Error().Message();

    // Step 5: Bootstrap through Atlas.RuntimeTest.dll's RunBootstrap forwarder.
    // Using clr_bootstrap(host, runtime_dll) would load Atlas.Runtime.dll as a
    // separate Assembly, giving it its own ErrorBridge statics.  RunBootstrap
    // keeps Atlas.Runtime loaded only as a dependency of Atlas.RuntimeTest,
    // so both share a single ErrorBridge.s_setError.
    (*s_dll_clear_error)();

    using BootstrapFn = int (*)(void*, void*);
    auto bootstrap_method = s_host.GetMethodAs<BootstrapFn>(test_dll(), kTestType, "RunBootstrap");
    ASSERT_TRUE(bootstrap_method.HasValue()) << bootstrap_method.Error().Message();

    ClrObjectVTableOut vtable_out{};
    const int rc = (*bootstrap_method)(static_cast<void*>(&args), static_cast<void*>(&vtable_out));
    ASSERT_EQ(rc, 0) << "RunBootstrap returned non-zero — check stderr";

    ASSERT_NE(vtable_out.free_handle, nullptr);

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
    ASSERT_TRUE(s_bootstrapped) << "CLR not bootstrapped — SetUpTestSuite failed";
    ClearClrError();
    if (s_dll_clear_error) s_dll_clear_error();
  }

  template <typename Ret, typename... Args>
  [[nodiscard]] auto bind_method(ClrStaticMethod<Ret, Args...>& method,
                                 std::string_view method_name) -> bool {
    auto result = method.Bind(s_host, test_dll(), kTestType, method_name);
    if (!result) {
      ADD_FAILURE() << "bind_method(" << method_name << ") failed: " << result.Error().Message();
      return false;
    }
    return true;
  }

  struct TestProvider : BaseNativeProvider {};

  static ClrHost s_host;
  static std::optional<DynamicLibrary> s_engine_lib;
  static bool s_bootstrapped;
  static TestProvider s_provider;

  // DLL-side TLS query helpers (populated in SetUpTestSuite).
  static int32_t (*s_dll_has_error)();
  static int32_t (*s_dll_read_error)(char*, int32_t);
  static void (*s_dll_clear_error)();
};

ClrHost ClrCallbackTest::s_host{};
std::optional<DynamicLibrary> ClrCallbackTest::s_engine_lib{};
bool ClrCallbackTest::s_bootstrapped = false;
ClrCallbackTest::TestProvider ClrCallbackTest::s_provider{};
int32_t (*ClrCallbackTest::s_dll_has_error)() = nullptr;
int32_t (*ClrCallbackTest::s_dll_read_error)(char*, int32_t) = nullptr;
void (*ClrCallbackTest::s_dll_clear_error)() = nullptr;

// ============================================================================
// Test 1: ABI version round-trip
// ============================================================================

TEST_F(ClrCallbackTest, AbiVersionRoundTrip) {
  ClrStaticMethod<int, uint32_t*> m;
  ASSERT_TRUE(bind_method(m, "GetAbiVersion"));

  uint32_t version = 0;
  auto res = m.Invoke(&version);
  ASSERT_TRUE(res.HasValue()) << res.Error().Message();
  EXPECT_EQ(*res, 0);
  EXPECT_EQ(version, kAtlasAbiVersion);
}

// ============================================================================
// Test 2: LogMessage doesn't crash
// ============================================================================

TEST_F(ClrCallbackTest, LogMessageDoesNotCrash) {
  ClrStaticMethod<int, const uint8_t*, int> m;
  ASSERT_TRUE(bind_method(m, "LogTestMessage"));

  const char msg[] = "callback-test-log-message";
  auto res = m.Invoke(reinterpret_cast<const uint8_t*>(msg), static_cast<int>(sizeof(msg) - 1));
  ASSERT_TRUE(res.HasValue()) << res.Error().Message();
  EXPECT_EQ(*res, 0);
}

// ============================================================================
// Test 3: ServerTime returns >= 0.0
// ============================================================================

TEST_F(ClrCallbackTest, ServerTimeIsNonNegative) {
  ClrStaticMethod<int, double*> m;
  ASSERT_TRUE(bind_method(m, "GetServerTime"));

  double t = -1.0;
  auto res = m.Invoke(&t);
  ASSERT_TRUE(res.HasValue()) << res.Error().Message();
  EXPECT_EQ(*res, 0);
  EXPECT_GE(t, 0.0);
}

// ============================================================================
// Test 4: Error bridge round-trip
// ============================================================================

TEST_F(ClrCallbackTest, ErrorBridgeRelaysException) {
  ASSERT_NE(s_dll_has_error, nullptr);

  // Call ThrowException via raw function pointer.  We bypass ClrStaticMethod
  // because its invoke() checks the LOCAL TLS (test binary copy), but the
  // error was written to the DLL's TLS.
  auto fn_result = s_host.GetMethod(test_dll(), kTestType, "ThrowException");
  ASSERT_TRUE(fn_result.HasValue()) << fn_result.Error().Message();

  using Fn = int (*)();
  const int ret = reinterpret_cast<Fn>(*fn_result)();

  EXPECT_EQ(ret, -1);
  EXPECT_EQ(s_dll_has_error(), 1);

  char msg_buf[256]{};
  (void)s_dll_read_error(msg_buf, static_cast<int32_t>(sizeof(msg_buf) - 1));
  EXPECT_NE(std::string_view(msg_buf).find("test-exception-message"), std::string_view::npos);
  EXPECT_EQ(s_dll_has_error(), 0);
}

// ============================================================================
// Test 5: GCHandle lifecycle via ClrObject
// ============================================================================

TEST_F(ClrCallbackTest, GCHandleLifecycle) {
  ClrStaticMethod<int, void**> m;
  ASSERT_TRUE(bind_method(m, "AllocStringHandle"));

  void* handle = nullptr;
  auto res = m.Invoke(&handle);
  ASSERT_TRUE(res.HasValue()) << res.Error().Message();
  EXPECT_EQ(*res, 0);
  ASSERT_NE(handle, nullptr);

  // Wrap in ClrObject — takes ownership of the GCHandle.
  {
    ClrObject obj{handle};
    EXPECT_FALSE(obj.IsNone());
    EXPECT_EQ(obj.TypeName(), "String");

    auto str_result = obj.AsString();
    ASSERT_TRUE(str_result.HasValue()) << str_result.Error().Message();
    EXPECT_EQ(*str_result, "hello from managed");
  }
  // ClrObject destructor calls GCHandleHelper.FreeHandle — no leak.
}

// ============================================================================
// Test 6: ProcessPrefix round-trip
// ============================================================================

TEST_F(ClrCallbackTest, ProcessPrefixRoundTrip) {
  ClrStaticMethod<int, uint8_t*> m;
  ASSERT_TRUE(bind_method(m, "GetProcessPrefix"));

  uint8_t prefix = 0xFF;
  auto res = m.Invoke(&prefix);
  ASSERT_TRUE(res.HasValue()) << res.Error().Message();
  EXPECT_EQ(*res, 0);
  EXPECT_EQ(prefix, 0);  // Stub returns 0 in Phase 2.
}

}  // namespace atlas::test
