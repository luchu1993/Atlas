#include "server/entity_app.hpp"

#include "foundation/log.hpp"
#include "network/network_interface.hpp"

namespace atlas
{

EntityApp::EntityApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ScriptApp(dispatcher, network), bg_task_manager_(dispatcher)
{
}

EntityApp::~EntityApp() = default;

auto EntityApp::init(int argc, char* argv[]) -> bool
{
    if (!ScriptApp::init(argc, argv))
        return false;

    // Open RUDP server on the internal port so peer services (LoginApp, CellApp,
    // other BaseApps) can connect to this process via RUDP.
    const auto& cfg = config();
    if (cfg.internal_port > 0)
    {
        Address listen_addr(0, cfg.internal_port);
        if (auto r =
                network().start_rudp_server(listen_addr, NetworkInterface::cluster_rudp_profile());
            !r)
        {
            ATLAS_LOG_ERROR("EntityApp: failed to start RUDP server on port {}: {}",
                            cfg.internal_port, r.error().message());
            return false;
        }
        ATLAS_LOG_INFO("EntityApp({}): RUDP server on {}", cfg.process_name,
                       network().rudp_address().to_string());
    }

    // Install User1 handler for stack-trace dumps (Linux: map SIGQUIT → SIGUSR1).
    install_signal_handler(Signal::User1, [this](Signal s) { on_signal(s); });

    return true;
}

void EntityApp::fini()
{
    remove_signal_handler(Signal::User1);
    ScriptApp::fini();
}

void EntityApp::on_start_of_tick()
{
    // Script-side timers are driven by ScriptEngine::on_tick() in
    // ScriptApp::on_tick_complete(), so nothing additional is needed here.
    // Subclasses (BaseApp, CellApp) use this hook for entity-level bookkeeping.
}

void EntityApp::on_signal(Signal sig)
{
    if (sig == Signal::User1)
    {
        // Diagnostic stack-trace dump requested (e.g. from Manager detecting a hung process).
        // On Linux this is triggered by SIGUSR1 (or via SIGQUIT mapped to SIGUSR1).
        ATLAS_LOG_CRITICAL("EntityApp: received diagnostic signal — dumping state");
        // platform::print_stack_trace() will be added when the platform library
        // gains stack-unwinding support. For now log a placeholder.
        ATLAS_LOG_CRITICAL("  game_time={}  uptime={:.1f}s  bg_tasks_pending={}", game_time(),
                           uptime_seconds(), bg_task_manager_.pending_count());
        // Do NOT call shutdown() — let the Manager decide what to do next.
        return;
    }
    ScriptApp::on_signal(sig);
}

void EntityApp::register_watchers()
{
    ScriptApp::register_watchers();

    auto& w = watcher_registry();
    w.add<uint32_t>("bg_tasks/pending", [this]() { return bg_task_manager_.pending_count(); });
    w.add<uint32_t>("bg_tasks/in_flight", [this]() { return bg_task_manager_.in_flight_count(); });
}

}  // namespace atlas
