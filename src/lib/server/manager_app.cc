#include "server/manager_app.h"

#include "foundation/log.h"
#include "network/network_interface.h"

namespace atlas {

ManagerApp::ManagerApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ServerApp(dispatcher, network) {}

auto ManagerApp::Init(int argc, char* argv[]) -> bool {
  if (!ServerApp::Init(argc, argv)) return false;

  const auto& cfg = Config();

  // Machined manages its own TCP server; all other manager processes open a
  // RUDP server on the internal port so peer services can connect via RUDP.
  if (cfg.process_type != ProcessType::kMachined && cfg.internal_port > 0) {
    Address listen_addr(0, cfg.internal_port);
    if (auto r = Network().StartRudpServer(listen_addr, NetworkInterface::ClusterRudpProfile());
        !r) {
      ATLAS_LOG_ERROR("ManagerApp: failed to start RUDP server on port {}: {}", cfg.internal_port,
                      r.Error().Message());
      return false;
    }
    ATLAS_LOG_INFO("ManagerApp({}): RUDP server on {}", cfg.process_name,
                   Network().RudpAddress().ToString());
  }

  return true;
}

void ManagerApp::RegisterWatchers() {
  ServerApp::RegisterWatchers();
  // Manager-specific watchers added here in concrete subclasses.
}

}  // namespace atlas
