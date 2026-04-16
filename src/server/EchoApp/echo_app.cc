#include "echo_app.h"

#include "foundation/log.h"

namespace atlas {

EchoApp::EchoApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

auto EchoApp::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  ATLAS_LOG_INFO("EchoApp initialised (update_hertz={})", Config().update_hertz);
  return true;
}

void EchoApp::OnTickComplete() {
  ++tick_count_;

  // Log a heartbeat every 10 ticks so output is visible but not overwhelming
  if (tick_count_ % 10 == 0) {
    ATLAS_LOG_DEBUG("EchoApp tick={} uptime={:.1f}s", tick_count_, UptimeSeconds());
  }
}

void EchoApp::Fini() {
  ATLAS_LOG_INFO("EchoApp shutting down after {} ticks", tick_count_);
  ManagerApp::Fini();
}

}  // namespace atlas
