#include "clrscript/base_native_provider.hpp"
#include "clrscript/clr_bootstrap.hpp"
#include "clrscript/clr_error.hpp"
#include "clrscript/clr_export.hpp"
#include "clrscript/clr_host.hpp"
#include "clrscript/clr_invoke.hpp"
#include "platform/dynamic_library.hpp"
#include "platform/platform_config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

// ============================================================================
// Path helpers (injected via CMake compile definitions)
// ============================================================================

namespace
{

std::filesystem::path runtime_config()
{
#ifdef ATLAS_SOURCE_DIR
    return std::filesystem::path(ATLAS_SOURCE_DIR) / "runtime" / "atlas_server.runtimeconfig.json";
#else
    return "runtime/atlas_server.runtimeconfig.json";
#endif
}

std::filesystem::path test_dll()
{
#ifdef ATLAS_BINARY_DIR
    return std::filesystem::path(ATLAS_BINARY_DIR) / "csharp" / "tests" / "csharp" /
           "Atlas.RuntimeTest" / "Atlas.RuntimeTest.dll";
#else
    return "csharp/tests/csharp/Atlas.RuntimeTest/Atlas.RuntimeTest.dll";
#endif
}

std::filesystem::path atlas_engine_path()
{
#ifdef ATLAS_RUNTIME_TEST_DIR
    std::filesystem::path dir = ATLAS_RUNTIME_TEST_DIR;
#elif defined(ATLAS_ENGINE_DIR)
    std::filesystem::path dir = ATLAS_ENGINE_DIR;
#else
    std::filesystem::path dir = ".";
#endif

#if ATLAS_PLATFORM_WINDOWS
    return dir / "atlas_engine.dll";
#else
    return dir / "libatlas_engine.so";
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

namespace atlas::test
{

class ClrScriptEngineTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        auto lib = DynamicLibrary::load(atlas_engine_path());
        ASSERT_TRUE(lib.has_value()) << "Cannot load atlas_engine: " << lib.error().message()
                                     << "\nPath: " << atlas_engine_path().string();
        s_engine_lib = std::move(*lib);

        using SetProviderFn = void (*)(void*);
        auto set_fn = s_engine_lib->get_symbol<SetProviderFn>("atlas_set_native_api_provider");
        ASSERT_TRUE(set_fn.has_value()) << set_fn.error().message();
        (*set_fn)(&s_provider);

        using GetFnPtr = void* (*)();
        using ErrorSetFn = void (*)(int32_t, const char*, int32_t);
        using ErrorClearFn = void (*)();
        using ErrorGetCodeFn = int32_t (*)();

        auto get_set = s_engine_lib->get_symbol<GetFnPtr>("atlas_get_clr_error_set_fn");
        auto get_clear = s_engine_lib->get_symbol<GetFnPtr>("atlas_get_clr_error_clear_fn");
        auto get_code = s_engine_lib->get_symbol<GetFnPtr>("atlas_get_clr_error_get_code_fn");
        ASSERT_TRUE(get_set.has_value() && get_clear.has_value() && get_code.has_value());

        ClrBootstrapArgs args;
        args.error_set = reinterpret_cast<ErrorSetFn>((*get_set)());
        args.error_clear = reinterpret_cast<ErrorClearFn>((*get_clear)());
        args.error_get_code = reinterpret_cast<ErrorGetCodeFn>((*get_code)());

        auto init = s_host.initialize(runtime_config());
        ASSERT_TRUE(init.has_value()) << "ClrHost init failed: " << init.error().message();

        using BootstrapFn = int (*)(void*, void*);
        auto bootstrap_method =
            s_host.get_method_as<BootstrapFn>(test_dll(), kCallbackType, "RunBootstrap");
        ASSERT_TRUE(bootstrap_method.has_value()) << bootstrap_method.error().message();

        ClrObjectVTableOut vtable_out{};
        const int rc =
            (*bootstrap_method)(static_cast<void*>(&args), static_cast<void*>(&vtable_out));
        ASSERT_EQ(rc, 0) << "RunBootstrap returned non-zero";

        ClrObjectVTable vtable{};
        vtable.free_handle = vtable_out.free_handle;
        vtable.get_type_name = vtable_out.get_type_name;
        vtable.is_none = vtable_out.is_none;
        vtable.to_int64 = vtable_out.to_int64;
        vtable.to_double = vtable_out.to_double;
        vtable.to_string = vtable_out.to_string;
        vtable.to_bool = vtable_out.to_bool;
        set_clr_object_vtable(vtable);

        s_bootstrapped = true;
    }

    static void TearDownTestSuite()
    {
        s_host.finalize();
        s_engine_lib.reset();
        s_bootstrapped = false;
    }

protected:
    void SetUp() override
    {
        ASSERT_TRUE(s_bootstrapped) << "CLR not bootstrapped";
        clear_clr_error();
    }

    template <typename Ret, typename... Args>
    [[nodiscard]] auto bind(ClrStaticMethod<Ret, Args...>& method, std::string_view name,
                            std::string_view type = kLifecycleType) -> bool
    {
        auto result = method.bind(s_host, test_dll(), type, name);
        if (!result)
        {
            ADD_FAILURE() << "bind(" << name << ") failed: " << result.error().message();
            return false;
        }
        return true;
    }

    /// RAII helper: calls EngineInit on construction, EngineShutdown on destruction.
    struct EngineScope
    {
        ClrFallibleMethod<> init;
        ClrFallibleMethod<> shutdown;
        ClrScriptEngineTest& test;
        bool ok = false;

        explicit EngineScope(ClrScriptEngineTest& t) : test(t)
        {
            ok = test.bind(init, "EngineInit") && test.bind(shutdown, "EngineShutdown");
            if (ok)
            {
                auto r = init.invoke();
                ok = r.has_value();
                if (!ok)
                    ADD_FAILURE() << "EngineInit failed: " << r.error().message();
            }
        }
        ~EngineScope()
        {
            if (ok)
                (void)shutdown.invoke();
        }
        EngineScope(const EngineScope&) = delete;
        EngineScope& operator=(const EngineScope&) = delete;
    };

    struct TestProvider : BaseNativeProvider
    {
    };

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

TEST_F(ClrScriptEngineTest, FullLifecycle)
{
    EngineScope scope(*this);
    ASSERT_TRUE(scope.ok);

    ClrFallibleMethod<uint8_t> on_init;
    ClrFallibleMethod<float> on_tick;
    ClrFallibleMethod<> on_shutdown;

    ASSERT_TRUE(bind(on_init, "OnInit"));
    ASSERT_TRUE(bind(on_tick, "OnTick"));
    ASSERT_TRUE(bind(on_shutdown, "OnShutdown"));

    auto r2 = on_init.invoke(uint8_t{0});
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    for (int i = 0; i < 10; ++i)
    {
        auto r = on_tick.invoke(0.016f);
        ASSERT_TRUE(r.has_value()) << "on_tick frame " << i << ": " << r.error().message();
    }

    auto r4 = on_shutdown.invoke();
    ASSERT_TRUE(r4.has_value()) << r4.error().message();
}

// ============================================================================
// Test 2: Log.Info through Lifecycle path
// ============================================================================

TEST_F(ClrScriptEngineTest, LogInfoForwarding)
{
    ClrStaticMethod<int, const uint8_t*, int> log_info;
    ASSERT_TRUE(bind(log_info, "LogInfoTest"));

    const char msg[] = "lifecycle-log-test";
    auto r =
        log_info.invoke(reinterpret_cast<const uint8_t*>(msg), static_cast<int>(sizeof(msg) - 1));
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(*r, 0);
}

// ============================================================================
// Test 3: Time.DeltaTime through NativeApi
// ============================================================================

TEST_F(ClrScriptEngineTest, DeltaTimeQuery)
{
    ClrStaticMethod<int, float*> get_dt;
    ASSERT_TRUE(bind(get_dt, "GetDeltaTime"));

    float dt = -1.0f;
    auto r = get_dt.invoke(&dt);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_GE(dt, 0.0f);
}

// ============================================================================
// Test 4: EntityManager.Count is 0 after init (clean state via EngineScope)
// ============================================================================

TEST_F(ClrScriptEngineTest, EntityCountAfterInit)
{
    EngineScope scope(*this);
    ASSERT_TRUE(scope.ok);

    ClrStaticMethod<int, int*> get_count;
    ASSERT_TRUE(bind(get_count, "GetEntityCount"));

    int count = -1;
    auto r = get_count.invoke(&count);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(count, 0);
}

// ============================================================================
// Phase 6: Boundary tests
// ============================================================================

TEST_F(ClrScriptEngineTest, ReInitAfterShutdown)
{
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
        auto r = get_count.invoke(&count);
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_EQ(count, 0);
    }
}

}  // namespace atlas::test
