#pragma once

#include "clrscript/native_api_provider.hpp"
#include "script/script_engine.hpp"
#include "server/server_app.hpp"

#include <memory>

namespace atlas
{

// ============================================================================
// ScriptApp — ServerApp + C# scripting layer
//
// Replaces BigWorld's ScriptApp (Python interpreter) with ClrScriptEngine.
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

class ScriptApp : public ServerApp
{
public:
    ScriptApp(EventDispatcher& dispatcher, NetworkInterface& network);
    ~ScriptApp() override;

    [[nodiscard]] auto script_engine() -> ScriptEngine&
    {
        ATLAS_ASSERT(script_engine_ != nullptr);
        return *script_engine_;
    }

protected:
    // ---- ServerApp overrides ------------------------------------------------

    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;

    // Drive C# on_tick() after all Updatables have run.
    void on_tick_complete() override;

    // ---- ScriptApp subclass hooks -------------------------------------------

    // Create the per-process INativeApiProvider implementation.
    // Called before CLR initialisation; must NOT use any CLR APIs.
    // Default returns a BaseNativeProvider (stubs only); override for real behaviour.
    [[nodiscard]] virtual auto create_native_provider() -> std::unique_ptr<INativeApiProvider>;

    // Called after C# OnInit() completes successfully.
    virtual void on_script_ready() {}

    // ---- Hot-reload stub (completed in Script Phase 5) ----------------------

    // Trigger assembly reload: on_shutdown → reload → on_init(true) → on_script_ready.
    void reload_scripts();

private:
    std::unique_ptr<ScriptEngine> script_engine_;
    std::unique_ptr<INativeApiProvider> native_provider_;
    float last_dt_{0.0f};  // seconds, updated each tick
};

}  // namespace atlas
