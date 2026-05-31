#ifndef ATLAS_LIB_SERVER_MANAGER_APP_H_
#define ATLAS_LIB_SERVER_MANAGER_APP_H_

#include "server/reviver_subject.h"
#include "server/server_app.h"

namespace atlas {

class ManagerApp : public ServerApp {
 public:
  ManagerApp(EventDispatcher& dispatcher, NetworkInterface& network);

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;

  void RegisterWatchers() override;

  // Arbitrates which Reviver is the active monitor from their HealthProbe
  // pings; the per-manager OnHealthProbe feeds it and echoes the verdict.
  ReviverSubject reviver_subject_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MANAGER_APP_H_
