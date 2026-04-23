#ifndef ATLAS_LIB_SERVER_SCRIPT_APP_H_
#define ATLAS_LIB_SERVER_SCRIPT_APP_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clrscript/native_api_provider.h"
#include "platform/dynamic_library.h"
#include "script/script_engine.h"
#include "server/server_app.h"

namespace atlas {

// ============================================================================
// ScriptApp — ServerApp + C# scripting layer (ClrScriptEngine).
//
// Init sequence:
//   ServerApp::init()
//   → create_native_provider()          (subclass hook, CLR not yet started)
//   → set_native_api_provider(provider)
//   → configure ClrScriptEngine
//   → script_engine_->initialize()      (starts CoreCLR)
//   → script_engine_->load_module(...)  (loads Atlas.Runtime.dll)
//   → script_engine_->on_init(false)    (triggers C# OnInit)
//   → on_script_ready()                 (subclass hook)
//
// Fini sequence:
//   script_engine_->on_shutdown()
//   script_engine_->finalize()
//   set_native_api_provider(nullptr)
//   ServerApp::fini()
// ============================================================================

class ScriptApp : public ServerApp {
 public:
  ScriptApp(EventDispatcher& dispatcher, NetworkInterface& network);
  ~ScriptApp() override;

  [[nodiscard]] auto GetScriptEngine() -> class ScriptEngine& {
    ATLAS_ASSERT(script_engine_ != nullptr);
    return *script_engine_;
  }

 protected:
  // ---- ServerApp overrides ------------------------------------------------

  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;

  // Drive C# on_tick() after all Updatables have run.
  void OnTickComplete() override;

  // ---- ScriptApp subclass hooks -------------------------------------------

  // Create the per-process INativeApiProvider implementation.
  // Called before CLR initialisation; must NOT use any CLR APIs.
  // Default returns a BaseNativeProvider (stubs only); override for real behaviour.
  [[nodiscard]] virtual auto CreateNativeProvider() -> std::unique_ptr<INativeApiProvider>;

  // Called after C# OnInit() completes successfully.
  virtual void OnScriptReady() {}

  // ---- Hot-reload --------------------------------------------------------

  // Trigger assembly reload: on_shutdown → reload → on_init(true) → on_script_ready.
  void ReloadScripts();

  void ClearNativeApiError();
  [[nodiscard]] auto ConsumeNativeApiError() -> std::optional<std::string>;

 private:
  struct NativeApiErrorExports {
    using HasErrorFn = int32_t (*)();
    using ReadErrorFn = int32_t (*)(char*, int32_t);
    using ClearErrorFn = void (*)();

    HasErrorFn has_error{nullptr};
    ReadErrorFn read_error{nullptr};
    ClearErrorFn clear_error{nullptr};

    [[nodiscard]] auto IsValid() const -> bool {
      return has_error != nullptr && read_error != nullptr && clear_error != nullptr;
    }
  };

  std::optional<DynamicLibrary> native_api_library_;
  NativeApiErrorExports native_api_error_exports_;
  std::unique_ptr<ScriptEngine> script_engine_;
  std::unique_ptr<INativeApiProvider> native_provider_;
  float last_dt_{0.0f};  // seconds, updated each tick
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SCRIPT_APP_H_
