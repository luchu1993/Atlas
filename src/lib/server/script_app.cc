#include "server/script_app.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <format>

#include "clrscript/base_native_provider.h"
#include "clrscript/clr_bootstrap.h"
#include "clrscript/clr_native_api.h"
#include "clrscript/clr_script_engine.h"
#include "foundation/log.h"

namespace atlas {

// Concrete instantiation of BaseNativeProvider (constructor is protected).
// Used as the default provider when a subclass does not override
// create_native_provider().
class DefaultNativeProvider final : public BaseNativeProvider {};

ScriptApp::ScriptApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ServerApp(dispatcher, network) {}

ScriptApp::~ScriptApp() = default;

// ============================================================================
// Init
// ============================================================================

auto ScriptApp::Init(int argc, char* argv[]) -> bool {
  if (!ServerApp::Init(argc, argv)) return false;

  // 1. Create native API provider (must happen before CLR starts)
  native_provider_ = CreateNativeProvider();
  SetNativeApiProvider(native_provider_.get());

  using SetNativeApiProviderFn = void (*)(void*);
  using GetClrBridgeFn = void* (*)();
  using HasClrErrorFn = int32_t (*)();
  using ReadClrErrorFn = int32_t (*)(char*, int32_t);
  using ClearClrErrorFn = void (*)();

#if ATLAS_PLATFORM_WINDOWS
  constexpr auto kNativeApiModuleName = "atlas_engine.dll";
#else
  constexpr auto kNativeApiModuleName = "libatlas_engine.so";
#endif

  auto native_api_path = std::filesystem::absolute(argv[0]).parent_path() / kNativeApiModuleName;
  auto native_api_lib_result = DynamicLibrary::Load(native_api_path);
  if (!native_api_lib_result) {
    ATLAS_LOG_ERROR("ScriptApp: failed to load native API module '{}': {}",
                    native_api_path.string(), native_api_lib_result.Error().Message());
    return false;
  }
  native_api_library_ = std::move(*native_api_lib_result);

  auto set_provider_result =
      native_api_library_->GetSymbol<SetNativeApiProviderFn>("AtlasSetNativeApiProvider");
  if (!set_provider_result) {
    ATLAS_LOG_ERROR("ScriptApp: failed to resolve AtlasSetNativeApiProvider: {}",
                    set_provider_result.Error().Message());
    return false;
  }
  (*set_provider_result)(native_provider_.get());

  auto error_set_result = native_api_library_->GetSymbol<GetClrBridgeFn>("AtlasGetClrErrorSetFn");
  auto error_clear_result =
      native_api_library_->GetSymbol<GetClrBridgeFn>("AtlasGetClrErrorClearFn");
  auto error_code_result =
      native_api_library_->GetSymbol<GetClrBridgeFn>("AtlasGetClrErrorGetCodeFn");
  if (!error_set_result || !error_clear_result || !error_code_result) {
    ATLAS_LOG_ERROR("ScriptApp: failed to resolve DLL CLR error bridge exports");
    return false;
  }

  auto has_error_result = native_api_library_->GetSymbol<HasClrErrorFn>("AtlasHasClrError");
  auto read_error_result = native_api_library_->GetSymbol<ReadClrErrorFn>("AtlasReadClrError");
  auto clear_error_api_result =
      native_api_library_->GetSymbol<ClearClrErrorFn>("AtlasClearClrError");
  if (!has_error_result || !read_error_result || !clear_error_api_result) {
    ATLAS_LOG_ERROR("ScriptApp: failed to resolve DLL CLR error query exports");
    return false;
  }
  native_api_error_exports_.has_error = *has_error_result;
  native_api_error_exports_.read_error = *read_error_result;
  native_api_error_exports_.clear_error = *clear_error_api_result;
  ClearNativeApiError();

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
  clr_config.runtime_config_path = Config().runtime_config;
  clr_config.runtime_assembly_path = Config().script_assembly;
  clr_config.bootstrap_args = bootstrap_args;

  auto cfg_result = clr->Configure(clr_config);
  if (!cfg_result) {
    ATLAS_LOG_ERROR("ScriptApp: ClrScriptEngine configure failed: {}",
                    cfg_result.Error().Message());
    return false;
  }

  script_engine_ = std::move(clr);
  // 3. Start CoreCLR
  auto init_result = script_engine_->Initialize();
  if (!init_result) {
    ATLAS_LOG_ERROR("ScriptApp: script engine initialize failed: {}",
                    init_result.Error().Message());
    return false;
  }
  // 4. Load Atlas.Runtime.dll (and user assemblies if configured)
  if (!Config().script_assembly.empty()) {
    auto load_result = script_engine_->LoadModule(Config().script_assembly);
    if (!load_result) {
      ATLAS_LOG_ERROR("ScriptApp: load_module({}) failed: {}", Config().script_assembly.string(),
                      load_result.Error().Message());
      return false;
    }
  }

  // 5. Trigger C# OnInit
  script_engine_->OnInit(false);

  // 6. Subclass hook
  OnScriptReady();

  return true;
}

// ============================================================================
// Fini
// ============================================================================

void ScriptApp::Fini() {
  using SetNativeApiProviderFn = void (*)(void*);

  if (script_engine_) {
    script_engine_->OnShutdown();
    script_engine_->Finalize();
    script_engine_.reset();
  }

  if (native_api_library_) {
    auto set_provider_result =
        native_api_library_->GetSymbol<SetNativeApiProviderFn>("AtlasSetNativeApiProvider");
    if (set_provider_result) (*set_provider_result)(nullptr);
    native_api_library_.reset();
  }
  native_api_error_exports_ = NativeApiErrorExports{};

  SetNativeApiProvider(nullptr);
  native_provider_.reset();

  ServerApp::Fini();
}

// ============================================================================
// Tick
// ============================================================================

void ScriptApp::OnTickComplete() {
  if (script_engine_) {
    using namespace std::chrono;
    last_dt_ = static_cast<float>(duration_cast<Seconds>(GetGameClock().FrameDelta()).count());
    script_engine_->OnTick(last_dt_);
  }
}

// ============================================================================
// Subclass hooks
// ============================================================================

auto ScriptApp::CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> {
  return std::make_unique<DefaultNativeProvider>();
}

void ScriptApp::ReloadScripts() {
  // Stub — full hot-reload is not implemented yet.
  if (!script_engine_) return;

  ATLAS_LOG_INFO("ScriptApp: reloading scripts...");
  script_engine_->OnShutdown();
  // reload_module would go here once ClrScriptEngine exposes it
  script_engine_->OnInit(true);
  OnScriptReady();
}

void ScriptApp::ClearNativeApiError() {
  if (native_api_error_exports_.IsValid()) {
    native_api_error_exports_.clear_error();
  }
}

auto ScriptApp::ConsumeNativeApiError() -> std::optional<std::string> {
  if (!native_api_error_exports_.IsValid() || native_api_error_exports_.has_error() == 0) {
    return std::nullopt;
  }

  std::array<char, 1024> buffer{};
  const int32_t kCode =
      native_api_error_exports_.read_error(buffer.data(), static_cast<int32_t>(buffer.size() - 1));
  std::string message(buffer.data());
  if (message.empty()) {
    message = std::format("managed callback failed with CLR error code {}", kCode);
  } else {
    message = std::format("{} (CLR error code {})", message, kCode);
  }

  return message;
}

}  // namespace atlas
