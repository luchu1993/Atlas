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

  auto host_result = host_.Initialize(config_.runtime_config_path);
  if (!host_result)
    return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: CLR init failed: {}",
                                                      host_result.Error().Message())};

  auto bootstrap_result =
      config_.bootstrap_args
          ? ClrBootstrap(host_, config_.runtime_assembly_path, *config_.bootstrap_args)
          : ClrBootstrap(host_, config_.runtime_assembly_path);
  if (!bootstrap_result)
    return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: bootstrap failed: {}",
                                                      bootstrap_result.Error().Message())};

  const auto& asm_path = config_.runtime_assembly_path;

  auto bind = [&](auto& method, std::string_view type_name, std::string_view name) -> Result<void> {
    auto r = method.Bind(host_, asm_path, type_name, name);
    if (!r)
      return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: failed to bind {}: {}",
                                                        name, r.Error().Message())};
    return {};
  };

  if (!config_.lifecycle_type.empty()) {
    const std::string_view kLifecycle{config_.lifecycle_type};
    auto r = bind(engine_init_, kLifecycle, "EngineInit");
    if (!r) return r.Error();
    r = bind(engine_shutdown_, kLifecycle, "EngineShutdown");
    if (!r) return r.Error();
    r = bind(on_init_, kLifecycle, "OnInit");
    if (!r) return r.Error();
    r = bind(on_tick_, kLifecycle, "OnTick");
    if (!r) return r.Error();
    r = bind(on_shutdown_, kLifecycle, "OnShutdown");
    if (!r) return r.Error();
  }

  if (!config_.hotreload_type.empty()) {
    const std::string_view kHotReload{config_.hotreload_type};
    auto r = bind(load_scripts_, kHotReload, "LoadScripts");
    if (!r) return r.Error();
    r = bind(serialize_and_unload_, kHotReload, "SerializeAndUnload");
    if (!r) return r.Error();
    r = bind(load_and_restore_, kHotReload, "LoadAndRestore");
    if (!r) return r.Error();
  }

  if (engine_init_.IsBound()) {
    auto init_result = engine_init_.Invoke();
    if (!init_result)
      return Error{ErrorCode::kScriptError, std::format("ClrScriptEngine: EngineInit failed: {}",
                                                        init_result.Error().Message())};
  }

  committed = true;
  initialized_ = true;
  ATLAS_LOG_INFO("ClrScriptEngine initialized ({})", RuntimeName());
  return {};
}

void ClrScriptEngine::Finalize() {
  if (!initialized_) return;

  if (engine_shutdown_.IsBound()) {
    auto shutdown_result = engine_shutdown_.Invoke();
    if (!shutdown_result)
      ATLAS_LOG_ERROR("ClrScriptEngine: EngineShutdown failed: {}",
                      shutdown_result.Error().Message());
  }

  ResetAllMethods();
  host_.Finalize();
  initialized_ = false;
  ATLAS_LOG_INFO("ClrScriptEngine finalized");
}

auto ClrScriptEngine::LoadModule(const std::filesystem::path& path) -> Result<void> {
  if (!initialized_) return Error{ErrorCode::kInvalidArgument, "ClrScriptEngine not initialized"};
  if (!load_scripts_.IsBound()) {
    return Error{ErrorCode::kInvalidArgument,
                 "ClrScriptEngine::LoadModule: hotreload_type is empty, no LoadScripts entry "
                 "bound - hosts that skip HotReloadManager must load assemblies by other means"};
  }

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
  if (!initialized_ || !on_tick_.IsBound()) return;
  auto result = on_tick_.Invoke(dt);
  if (!result) ATLAS_LOG_ERROR("ClrScriptEngine::OnTick failed: {}", result.Error().Message());
}

void ClrScriptEngine::OnInit(bool is_reload) {
  if (!initialized_ || !on_init_.IsBound()) return;
  auto result = on_init_.Invoke(is_reload ? uint8_t{1} : uint8_t{0});
  if (!result) ATLAS_LOG_ERROR("ClrScriptEngine::OnInit failed: {}", result.Error().Message());
}

void ClrScriptEngine::OnShutdown() {
  if (!initialized_ || !on_shutdown_.IsBound()) return;
  auto result = on_shutdown_.Invoke();
  if (!result) ATLAS_LOG_ERROR("ClrScriptEngine::OnShutdown failed: {}", result.Error().Message());
}

auto ClrScriptEngine::CallFunction(std::string_view /*module_name*/,
                                   std::string_view /*function_name*/,
                                   std::span<const ScriptValue> /*args*/) -> Result<ScriptValue> {
  return Error{ErrorCode::kNotSupported,
               "CallFunction() not yet implemented - use lifecycle methods"};
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
