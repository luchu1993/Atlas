#include "server/script_app.hpp"

#include "clrscript/base_native_provider.hpp"
#include "clrscript/clr_bootstrap.hpp"
#include "clrscript/clr_native_api.hpp"
#include "clrscript/clr_script_engine.hpp"
#include "foundation/log.hpp"

#include <chrono>
#include <filesystem>

namespace atlas
{

// Concrete instantiation of BaseNativeProvider (constructor is protected).
// Used as the default provider when a subclass does not override
// create_native_provider().
class DefaultNativeProvider final : public BaseNativeProvider
{
};

ScriptApp::ScriptApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ServerApp(dispatcher, network)
{
}

ScriptApp::~ScriptApp() = default;

// ============================================================================
// Init
// ============================================================================

auto ScriptApp::init(int argc, char* argv[]) -> bool
{
    if (!ServerApp::init(argc, argv))
        return false;

    // 1. Create native API provider (must happen before CLR starts)
    native_provider_ = create_native_provider();
    set_native_api_provider(native_provider_.get());

    using SetNativeApiProviderFn = void (*)(void*);
    using GetClrBridgeFn = void* (*)();

#if ATLAS_PLATFORM_WINDOWS
    constexpr auto kNativeApiModuleName = "atlas_engine.dll";
#else
    constexpr auto kNativeApiModuleName = "libatlas_engine.so";
#endif

    auto native_api_path = std::filesystem::absolute(argv[0]).parent_path() / kNativeApiModuleName;
    auto native_api_lib_result = DynamicLibrary::load(native_api_path);
    if (!native_api_lib_result)
    {
        ATLAS_LOG_ERROR("ScriptApp: failed to load native API module '{}': {}",
                        native_api_path.string(), native_api_lib_result.error().message());
        return false;
    }
    native_api_library_ = std::move(*native_api_lib_result);

    auto set_provider_result =
        native_api_library_->get_symbol<SetNativeApiProviderFn>("atlas_set_native_api_provider");
    if (!set_provider_result)
    {
        ATLAS_LOG_ERROR("ScriptApp: failed to resolve atlas_set_native_api_provider: {}",
                        set_provider_result.error().message());
        return false;
    }
    (*set_provider_result)(native_provider_.get());

    auto error_set_result =
        native_api_library_->get_symbol<GetClrBridgeFn>("atlas_get_clr_error_set_fn");
    auto error_clear_result =
        native_api_library_->get_symbol<GetClrBridgeFn>("atlas_get_clr_error_clear_fn");
    auto error_code_result =
        native_api_library_->get_symbol<GetClrBridgeFn>("atlas_get_clr_error_get_code_fn");
    if (!error_set_result || !error_clear_result || !error_code_result)
    {
        ATLAS_LOG_ERROR("ScriptApp: failed to resolve DLL CLR error bridge exports");
        return false;
    }

    ClrBootstrapArgs bootstrap_args;
    bootstrap_args.error_set =
        reinterpret_cast<decltype(bootstrap_args.error_set)>((*error_set_result)());
    bootstrap_args.error_clear =
        reinterpret_cast<decltype(bootstrap_args.error_clear)>((*error_clear_result)());
    bootstrap_args.error_get_code =
        reinterpret_cast<decltype(bootstrap_args.error_get_code)>((*error_code_result)());

    // 2. Create and configure the CLR script engine
    auto clr = std::make_unique<ClrScriptEngine>();

    ClrScriptEngine::Config clr_config;
    clr_config.runtime_config_path = config().runtime_config;
    clr_config.runtime_assembly_path = config().script_assembly;
    clr_config.bootstrap_args = bootstrap_args;

    auto cfg_result = clr->configure(clr_config);
    if (!cfg_result)
    {
        ATLAS_LOG_ERROR("ScriptApp: ClrScriptEngine configure failed: {}",
                        cfg_result.error().message());
        return false;
    }

    script_engine_ = std::move(clr);
    // 3. Start CoreCLR
    auto init_result = script_engine_->initialize();
    if (!init_result)
    {
        ATLAS_LOG_ERROR("ScriptApp: script engine initialize failed: {}",
                        init_result.error().message());
        return false;
    }
    // 4. Load Atlas.Runtime.dll (and user assemblies if configured)
    if (!config().script_assembly.empty())
    {
        auto load_result = script_engine_->load_module(config().script_assembly);
        if (!load_result)
        {
            ATLAS_LOG_ERROR("ScriptApp: load_module({}) failed: {}",
                            config().script_assembly.string(), load_result.error().message());
            return false;
        }
    }

    // 5. Trigger C# OnInit
    script_engine_->on_init(false);

    // 6. Subclass hook
    on_script_ready();

    return true;
}

// ============================================================================
// Fini
// ============================================================================

void ScriptApp::fini()
{
    using SetNativeApiProviderFn = void (*)(void*);

    if (script_engine_)
    {
        script_engine_->on_shutdown();
        script_engine_->finalize();
        script_engine_.reset();
    }

    if (native_api_library_)
    {
        auto set_provider_result = native_api_library_->get_symbol<SetNativeApiProviderFn>(
            "atlas_set_native_api_provider");
        if (set_provider_result)
            (*set_provider_result)(nullptr);
        native_api_library_.reset();
    }

    set_native_api_provider(nullptr);
    native_provider_.reset();

    ServerApp::fini();
}

// ============================================================================
// Tick
// ============================================================================

void ScriptApp::on_tick_complete()
{
    if (script_engine_)
    {
        using namespace std::chrono;
        last_dt_ = static_cast<float>(duration_cast<Seconds>(game_clock().frame_delta()).count());
        script_engine_->on_tick(last_dt_);
    }
}

// ============================================================================
// Subclass hooks
// ============================================================================

auto ScriptApp::create_native_provider() -> std::unique_ptr<INativeApiProvider>
{
    return std::make_unique<DefaultNativeProvider>();
}

void ScriptApp::reload_scripts()
{
    // Hot-reload stub — full implementation in Script Phase 5.
    if (!script_engine_)
        return;

    ATLAS_LOG_INFO("ScriptApp: reloading scripts...");
    script_engine_->on_shutdown();
    // reload_module would go here once ClrScriptEngine exposes it
    script_engine_->on_init(true);
    on_script_ready();
}

}  // namespace atlas
