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

    // RAII rollback: on any failure, reset methods and shut down the CLR host.
    bool committed = false;
    struct Rollback
    {
        ClrScriptEngine& self;
        bool& committed;
        ~Rollback()
        {
            if (!committed)
            {
                self.reset_all_methods();
                self.host_.finalize();
            }
        }
    } rollback{*this, committed};

    // 1. Start the CLR host.
    auto host_result = host_.initialize(config_.runtime_config_path);
    if (!host_result)
        return Error{ErrorCode::ScriptError, std::format("ClrScriptEngine: CLR init failed: {}",
                                                         host_result.error().message())};

    // 2. Run Phase 2 bootstrap (error bridge + vtable).
    auto bootstrap_result =
        config_.bootstrap_args
            ? clr_bootstrap(host_, config_.runtime_assembly_path, *config_.bootstrap_args)
            : clr_bootstrap(host_, config_.runtime_assembly_path);
    if (!bootstrap_result)
        return Error{ErrorCode::ScriptError, std::format("ClrScriptEngine: bootstrap failed: {}",
                                                         bootstrap_result.error().message())};

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
        return r.error();
    r = bind(engine_shutdown_, "EngineShutdown");
    if (!r)
        return r.error();
    r = bind(on_init_, "OnInit");
    if (!r)
        return r.error();
    r = bind(on_tick_, "OnTick");
    if (!r)
        return r.error();
    r = bind(on_shutdown_, "OnShutdown");
    if (!r)
        return r.error();

    // 3b. Bind HotReloadManager methods.
    auto bind_hr = [&](auto& method, std::string_view name) -> Result<void>
    {
        auto hr = method.bind(host_, asm_path, kHotReloadType, name);
        if (!hr)
            return Error{
                ErrorCode::ScriptError,
                std::format("ClrScriptEngine: failed to bind {}: {}", name, hr.error().message())};
        return {};
    };

    r = bind_hr(load_scripts_, "LoadScripts");
    if (!r)
        return r.error();
    r = bind_hr(serialize_and_unload_, "SerializeAndUnload");
    if (!r)
        return r.error();
    r = bind_hr(load_and_restore_, "LoadAndRestore");
    if (!r)
        return r.error();

    // 4. Call C# EngineInit (sets up EngineContext, EntityManager, etc.)
    auto init_result = engine_init_.invoke();
    if (!init_result)
        return Error{ErrorCode::ScriptError, std::format("ClrScriptEngine: EngineInit failed: {}",
                                                         init_result.error().message())};

    committed = true;
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

auto ClrScriptEngine::load_module(const std::filesystem::path& path) -> Result<void>
{
    if (!initialized_)
        return Error{ErrorCode::InvalidArgument, "ClrScriptEngine not initialized"};

    auto path_str = path.u8string();
    auto result = load_scripts_.invoke(reinterpret_cast<const uint8_t*>(path_str.data()),
                                       static_cast<int32_t>(path_str.size()));
    if (!result)
        return Error{ErrorCode::ScriptError,
                     std::format("load_module failed: {}", result.error().message())};
    ATLAS_LOG_INFO("ClrScriptEngine: loaded script module {}", path.string());
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
    return Error{ErrorCode::NotSupported,
                 "call_function() not yet implemented — use lifecycle methods"};
}

auto ClrScriptEngine::call_hot_reload(std::string_view method_name) -> Result<void>
{
    if (!initialized_)
        return Error{ErrorCode::InvalidArgument, "ClrScriptEngine not initialized"};

    if (method_name == "SerializeAndUnload")
    {
        auto r = serialize_and_unload_.invoke();
        if (!r)
            return r.error();
        return {};
    }

    return Error{ErrorCode::InvalidArgument,
                 std::format("Unknown hot-reload method: {}", method_name)};
}

auto ClrScriptEngine::call_hot_reload(std::string_view method_name,
                                      const std::filesystem::path& assembly_path) -> Result<void>
{
    if (!initialized_)
        return Error{ErrorCode::InvalidArgument, "ClrScriptEngine not initialized"};

    if (method_name == "LoadAndRestore")
    {
        auto path_str = assembly_path.u8string();
        auto r = load_and_restore_.invoke(reinterpret_cast<const uint8_t*>(path_str.data()),
                                          static_cast<int32_t>(path_str.size()));
        if (!r)
            return r.error();
        return {};
    }

    return Error{ErrorCode::InvalidArgument,
                 std::format("Unknown hot-reload method: {}", method_name)};
}

void ClrScriptEngine::reset_all_methods()
{
    engine_init_.reset();
    engine_shutdown_.reset();
    on_init_.reset();
    on_tick_.reset();
    on_shutdown_.reset();
    load_scripts_.reset();
    serialize_and_unload_.reset();
    load_and_restore_.reset();
}

}  // namespace atlas
