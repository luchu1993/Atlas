#ifndef ATLAS_LIB_SERVER_ENTITY_APP_H_
#define ATLAS_LIB_SERVER_ENTITY_APP_H_

#include "entitydef/entity_def_registry.h"
#include "network/bg_task_manager.h"
#include "platform/signal_handler.h"
#include "server/script_app.h"

namespace atlas {

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

  void OnSignal(Signal sig) override;

  void RegisterWatchers() override;

 private:
  BgTaskManager bg_task_manager_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_ENTITY_APP_H_
