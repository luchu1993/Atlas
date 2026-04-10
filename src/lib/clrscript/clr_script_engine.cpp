#include "clrscript/clr_script_engine.hpp"

#include "foundation/log.hpp"

#include <format>

namespace atlas
{

auto ClrScriptEngine::configure(const Config& config) -> Result<void>
{
    if (initialized_)
        return Error{ErrorCode::InvalidArgument, "Cannot configure after initialization"};
    config_ = config;
    configured_ = true;
    return {};
}

auto ClrScriptEngine::initialize() -> Result<void>
{
    if (initialized_)
        return Error{ErrorCode::AlreadyExists, "ClrScriptEngine already initialized"};
    if (!configured_)
        return Error{ErrorCode::InvalidArgument, "configure() must be called before initialize()"};

    // 1. Start the CLR host.
    auto host_result = host_.initialize(config_.runtime_config_path);
    if (!host_result)
        return Error{ErrorCode::ScriptError, std::format("ClrScriptEngine: CLR init failed: {}",
                                                         host_result.error().message())};

    // 2. Run Phase 2 bootstrap (error bridge + vtable).
    auto bootstrap_result = clr_bootstrap(host_, config_.runtime_assembly_path);
    if (!bootstrap_result)
    {
        host_.finalize();
        return Error{ErrorCode::ScriptError, std::format("ClrScriptEngine: bootstrap failed: {}",
                                                         bootstrap_result.error().message())};
    }

    // 3. Bind Phase 3 lifecycle methods.
    const auto& asm_path = config_.runtime_assembly_path;

    auto bind = [&](auto& method, std::string_view name) -> Result<void>
    {
        auto r = method.bind(host_, asm_path, kLifecycleType, name);
        if (!r)
            return Error{
                ErrorCode::ScriptError,
                std::format("ClrScriptEngine: failed to bind {}: {}", name, r.error().message())};
        return {};
    };

    auto r = bind(engine_init_, "EngineInit");
    if (!r)
    {
        reset_all_methods();
        host_.finalize();
        return r.error();
    }

    r = bind(engine_shutdown_, "EngineShutdown");
    if (!r)
    {
        reset_all_methods();
        host_.finalize();
        return r.error();
    }

    r = bind(on_init_, "OnInit");
    if (!r)
    {
        reset_all_methods();
        host_.finalize();
        return r.error();
    }

    r = bind(on_tick_, "OnTick");
    if (!r)
    {
        reset_all_methods();
        host_.finalize();
        return r.error();
    }

    r = bind(on_shutdown_, "OnShutdown");
    if (!r)
    {
        reset_all_methods();
        host_.finalize();
        return r.error();
    }

    // 4. Call C# EngineInit (sets up EngineContext, EntityManager, etc.)
    auto init_result = engine_init_.invoke();
    if (!init_result)
    {
        reset_all_methods();
        host_.finalize();
        return Error{ErrorCode::ScriptError, std::format("ClrScriptEngine: EngineInit failed: {}",
                                                         init_result.error().message())};
    }

    initialized_ = true;
    ATLAS_LOG_INFO("ClrScriptEngine initialized ({})", runtime_name());
    return {};
}

void ClrScriptEngine::finalize()
{
    if (!initialized_)
        return;

    auto shutdown_result = engine_shutdown_.invoke();
    if (!shutdown_result)
        ATLAS_LOG_ERROR("ClrScriptEngine: EngineShutdown failed: {}",
                        shutdown_result.error().message());

    reset_all_methods();
    host_.finalize();
    initialized_ = false;
    ATLAS_LOG_INFO("ClrScriptEngine finalized");
}

auto ClrScriptEngine::load_module(const std::filesystem::path& /*path*/) -> Result<void>
{
    // Phase 3: game-script assembly loading is deferred to Phase 5 (hot-reload).
    // The current engine uses Atlas.Runtime.dll as the sole managed assembly.
    // load_module() is a no-op placeholder for future extension.
    return {};
}

void ClrScriptEngine::on_tick(float dt)
{
    if (!initialized_)
        return;
    auto result = on_tick_.invoke(dt);
    if (!result)
        ATLAS_LOG_ERROR("ClrScriptEngine::on_tick failed: {}", result.error().message());
}

void ClrScriptEngine::on_init(bool is_reload)
{
    if (!initialized_)
        return;
    auto result = on_init_.invoke(is_reload ? uint8_t{1} : uint8_t{0});
    if (!result)
        ATLAS_LOG_ERROR("ClrScriptEngine::on_init failed: {}", result.error().message());
}

void ClrScriptEngine::on_shutdown()
{
    if (!initialized_)
        return;
    auto result = on_shutdown_.invoke();
    if (!result)
        ATLAS_LOG_ERROR("ClrScriptEngine::on_shutdown failed: {}", result.error().message());
}

auto ClrScriptEngine::call_function(std::string_view /*module_name*/,
                                    std::string_view /*function_name*/,
                                    std::span<const ScriptValue> /*args*/) -> Result<ScriptValue>
{
    // Phase 3: dynamic function dispatch is deferred to Phase 4 (Source Generator).
    // The primary C++ → C# path uses the lifecycle methods above.
    return Error{ErrorCode::NotSupported,
                 "call_function() not yet implemented — use lifecycle methods"};
}

void ClrScriptEngine::reset_all_methods()
{
    engine_init_.reset();
    engine_shutdown_.reset();
    on_init_.reset();
    on_tick_.reset();
    on_shutdown_.reset();
}

}  // namespace atlas
