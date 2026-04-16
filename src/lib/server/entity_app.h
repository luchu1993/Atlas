#ifndef ATLAS_LIB_SERVER_ENTITY_APP_H_
#define ATLAS_LIB_SERVER_ENTITY_APP_H_

#include "entitydef/entity_def_registry.h"
#include "network/bg_task_manager.h"
#include "platform/signal_handler.h"
#include "server/script_app.h"

namespace atlas {

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

class EntityApp : public ScriptApp {
 public:
  EntityApp(EventDispatcher& dispatcher, NetworkInterface& network);
  ~EntityApp() override;

  [[nodiscard]] auto GetBgTaskManager() -> class BgTaskManager& { return bg_task_manager_; }
  [[nodiscard]] auto EntityDefs() -> EntityDefRegistry& { return EntityDefRegistry::Instance(); }

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;

  void OnStartOfTick() override;

  // User1 (maps to SIGQUIT on Linux) → log call-stack for hung-process diagnosis.
  void OnSignal(Signal sig) override;

  void RegisterWatchers() override;

 private:
  BgTaskManager bg_task_manager_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_ENTITY_APP_H_
