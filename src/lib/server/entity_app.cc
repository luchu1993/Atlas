#include "server/entity_app.h"

#include "foundation/log.h"
#include "network/network_interface.h"

namespace atlas {

EntityApp::EntityApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ScriptApp(dispatcher, network), bg_task_manager_(dispatcher) {}

EntityApp::~EntityApp() = default;

auto EntityApp::Init(int argc, char* argv[]) -> bool {
  if (!ScriptApp::Init(argc, argv)) return false;

  // Open RUDP server on the internal port so peer services (LoginApp, CellApp,
  // other BaseApps) can connect to this process via RUDP.
  const auto& cfg = Config();
  if (cfg.internal_port > 0) {
    Address listen_addr(0, cfg.internal_port);
    if (auto r = Network().StartRudpServer(listen_addr, NetworkInterface::ClusterRudpProfile());
        !r) {
      ATLAS_LOG_ERROR("EntityApp: failed to start RUDP server on port {}: {}", cfg.internal_port,
                      r.Error().Message());
      return false;
    }
    ATLAS_LOG_INFO("EntityApp({}): RUDP server on {}", cfg.process_name,
                   Network().RudpAddress().ToString());
  }

  // Install User1 handler for stack-trace dumps (Linux: map SIGQUIT → SIGUSR1).
  InstallSignalHandler(Signal::kUser1, [this](Signal s) { OnSignal(s); });

  return true;
}

void EntityApp::Fini() {
  RemoveSignalHandler(Signal::kUser1);
  ScriptApp::Fini();
}

void EntityApp::OnStartOfTick() {
  // Script-side timers are driven by ScriptEngine::on_tick() in
  // ScriptApp::on_tick_complete(), so nothing additional is needed here.
  // Subclasses (BaseApp, CellApp) use this hook for entity-level bookkeeping.
}

void EntityApp::OnSignal(Signal sig) {
  if (sig == Signal::kUser1) {
    // Diagnostic stack-trace dump requested (e.g. from Manager detecting a hung process).
    // On Linux this is triggered by SIGUSR1 (or via SIGQUIT mapped to SIGUSR1).
    ATLAS_LOG_CRITICAL("EntityApp: received diagnostic signal — dumping state");
    // platform::print_stack_trace() will be added when the platform library
    // gains stack-unwinding support. For now log a placeholder.
    ATLAS_LOG_CRITICAL("  game_time={}  uptime={:.1f}s  bg_tasks_pending={}", GameTime(),
                       UptimeSeconds(), bg_task_manager_.PendingCount());
    // Do NOT call shutdown() — let the Manager decide what to do next.
    return;
  }
  ScriptApp::OnSignal(sig);
}

void EntityApp::RegisterWatchers() {
  ScriptApp::RegisterWatchers();

  auto& w = GetWatcherRegistry();
  w.Add<uint32_t>("bg_tasks/pending", [this]() { return bg_task_manager_.PendingCount(); });
  w.Add<uint32_t>("bg_tasks/in_flight", [this]() { return bg_task_manager_.InFlightCount(); });
}

}  // namespace atlas
