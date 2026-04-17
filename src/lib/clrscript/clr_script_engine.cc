#include "clrscript/clr_script_engine.h"

#include <format>

#include "foundation/log.h"

namespace atlas {

auto ClrScriptEngine::Configure(const Config& config) -> Result<void> {
  if (initialized_)
    return Error{ErrorCode::kInvalidArgument, "Cannot configure after initialization"};
  config_ = config;
  configured_ = true;
  return {};
}

auto ClrScriptEngine::Initialize() -> Result<void> {
  if (initialized_) return Error{ErrorCode::kAlreadyExists, "ClrScriptEngine already initialized"};
  if (!configured_)
    return Error{ErrorCode::kInvalidArgument, "Configure() must be called before Initialize()"};

  // RAII rollback: on any failure, reset methods and shut down the CLR host.
  bool committed = false;
  struct Rollback {
    ClrScriptEngine& self;
    bool& committed;
    ~Rollback() {
      if (!committed) {
        self.ResetAllMethods();
        self.host_.Finalize();
      }
    }
  } rollback{*this, committed};

  // 1. Start the CLR host.
  auto host_result = host_.Initialize(config_.runtime_config_path);
  if (!host_result)
    return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: CLR init failed: {}",
                                                      host_result.Error().Message())};

  // 2. Run Phase 2 bootstrap (error bridge + vtable).
  auto bootstrap_result =
      config_.bootstrap_args
          ? ClrBootstrap(host_, config_.runtime_assembly_path, *config_.bootstrap_args)
          : ClrBootstrap(host_, config_.runtime_assembly_path);
  if (!bootstrap_result)
    return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: bootstrap failed: {}",
                                                      bootstrap_result.Error().Message())};

  // 3. Bind Phase 3 lifecycle methods.
  const auto& asm_path = config_.runtime_assembly_path;

  auto bind = [&](auto& method, std::string_view name) -> Result<void> {
    auto r = method.Bind(host_, asm_path, kLifecycleType, name);
    if (!r)
      return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: failed to bind {}: {}",
                                                        name, r.Error().Message())};
    return {};
  };

  auto r = bind(engine_init_, "EngineInit");
  if (!r) return r.Error();
  r = bind(engine_shutdown_, "EngineShutdown");
  if (!r) return r.Error();
  r = bind(on_init_, "OnInit");
  if (!r) return r.Error();
  r = bind(on_tick_, "OnTick");
  if (!r) return r.Error();
  r = bind(on_shutdown_, "OnShutdown");
  if (!r) return r.Error();

  // 3b. Bind HotReloadManager methods.
  auto bind_hr = [&](auto& method, std::string_view name) -> Result<void> {
    auto hr = method.Bind(host_, asm_path, kHotReloadType, name);
    if (!hr)
      return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: failed to bind {}: {}",
                                                        name, hr.Error().Message())};
    return {};
  };

  r = bind_hr(load_scripts_, "LoadScripts");
  if (!r) return r.Error();
  r = bind_hr(serialize_and_unload_, "SerializeAndUnload");
  if (!r) return r.Error();
  r = bind_hr(load_and_restore_, "LoadAndRestore");
  if (!r) return r.Error();

  // 4. Call C# EngineInit (sets up EngineContext, EntityManager, etc.)
  auto init_result = engine_init_.Invoke();
  if (!init_result)
    return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: EngineInit failed: {}",
                                                      init_result.Error().Message())};

  committed = true;
  initialized_ = true;
  ATLAS_LOG_INFO("ClrScriptEngine initialized ({})", RuntimeName());
  return {};
}

void ClrScriptEngine::Finalize() {
  if (!initialized_) return;

  auto shutdown_result = engine_shutdown_.Invoke();
  if (!shutdown_result)
    ATLAS_LOG_ERROR("ClrScriptEngine: EngineShutdown failed: {}",
                    shutdown_result.Error().Message());

  ResetAllMethods();
  host_.Finalize();
  initialized_ = false;
  ATLAS_LOG_INFO("ClrScriptEngine finalized");
}

auto ClrScriptEngine::LoadModule(const std::filesystem::path& path) -> Result<void> {
  if (!initialized_) return Error{ErrorCode::kInvalidArgument, "ClrScriptEngine not initialized"};

  auto path_str = path.u8string();
  auto result = load_scripts_.Invoke(reinterpret_cast<const uint8_t*>(path_str.data()),
                                     static_cast<int32_t>(path_str.size()));
  if (!result)
    return Error{ErrorCode::kScriptError,
                 std::format("LoadModule failed: {}", result.Error().Message())};
  ATLAS_LOG_INFO("ClrScriptEngine: loaded script module {}", path.string());
  return {};
}

void ClrScriptEngine::OnTick(float dt) {
  if (!initialized_) return;
  auto result = on_tick_.Invoke(dt);
  if (!result) ATLAS_LOG_ERROR("ClrScriptEngine::OnTick failed: {}", result.Error().Message());
}

void ClrScriptEngine::OnInit(bool is_reload) {
  if (!initialized_) return;
  auto result = on_init_.Invoke(is_reload ? uint8_t{1} : uint8_t{0});
  if (!result) ATLAS_LOG_ERROR("ClrScriptEngine::OnInit failed: {}", result.Error().Message());
}

void ClrScriptEngine::OnShutdown() {
  if (!initialized_) return;
  auto result = on_shutdown_.Invoke();
  if (!result) ATLAS_LOG_ERROR("ClrScriptEngine::OnShutdown failed: {}", result.Error().Message());
}

auto ClrScriptEngine::CallFunction(std::string_view /*module_name*/,
                                   std::string_view /*function_name*/,
                                   std::span<const ScriptValue> /*args*/) -> Result<ScriptValue> {
  return Error{ErrorCode::kNotSupported,
               "CallFunction() not yet implemented — use lifecycle methods"};
}

auto ClrScriptEngine::CallHotReload(std::string_view method_name) -> Result<void> {
  if (!initialized_) return Error{ErrorCode::kInvalidArgument, "ClrScriptEngine not initialized"};

  if (method_name == "SerializeAndUnload") {
    auto r = serialize_and_unload_.Invoke();
    if (!r) return r.Error();
    return {};
  }

  return Error{ErrorCode::kInvalidArgument,
               std::format("Unknown hot-reload method: {}", method_name)};
}

auto ClrScriptEngine::CallHotReload(std::string_view method_name,
                                    const std::filesystem::path& assembly_path) -> Result<void> {
  if (!initialized_) return Error{ErrorCode::kInvalidArgument, "ClrScriptEngine not initialized"};

  if (method_name == "LoadAndRestore") {
    auto path_str = assembly_path.u8string();
    auto r = load_and_restore_.Invoke(reinterpret_cast<const uint8_t*>(path_str.data()),
                                      static_cast<int32_t>(path_str.size()));
    if (!r) return r.Error();
    return {};
  }

  return Error{ErrorCode::kInvalidArgument,
               std::format("Unknown hot-reload method: {}", method_name)};
}

void ClrScriptEngine::ResetAllMethods() {
  engine_init_.Reset();
  engine_shutdown_.Reset();
  on_init_.Reset();
  on_tick_.Reset();
  on_shutdown_.Reset();
  load_scripts_.Reset();
  serialize_and_unload_.Reset();
  load_and_restore_.Reset();
}

}  // namespace atlas
