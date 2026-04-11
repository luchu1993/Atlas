#pragma once

#include "entitydef/entity_def_registry.hpp"
#include "network/bg_task_manager.hpp"
#include "platform/signal_handler.hpp"
#include "server/script_app.hpp"

namespace atlas
{

// ============================================================================
// EntityApp — base class for entity-bearing server processes
//
// Extends ScriptApp with:
//   • BgTaskManager  — thread-pool for DB / IO work + main-thread callbacks
//   • EntityDefRegistry ref — entity type definitions (populated by C# at startup)
//   • on_start_of_tick() — drives script-side timers via ScriptEngine::on_tick
//   • on_signal(User1) — prints a stack-trace for "hung process" diagnosis.
//     On Linux, map SIGQUIT → SIGUSR1 at the process level to trigger this.
//
// Class hierarchy:
//   ServerApp → ScriptApp → EntityApp → BaseApp / CellApp
// ============================================================================

class EntityApp : public ScriptApp
{
public:
    EntityApp(EventDispatcher& dispatcher, NetworkInterface& network);
    ~EntityApp() override;

    [[nodiscard]] auto bg_task_manager() -> BgTaskManager& { return bg_task_manager_; }
    [[nodiscard]] auto entity_defs() -> EntityDefRegistry& { return EntityDefRegistry::instance(); }

protected:
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;

    void on_start_of_tick() override;

    // User1 (maps to SIGQUIT on Linux) → log call-stack for hung-process diagnosis.
    void on_signal(Signal sig) override;

    void register_watchers() override;

private:
    BgTaskManager bg_task_manager_;
};

}  // namespace atlas
