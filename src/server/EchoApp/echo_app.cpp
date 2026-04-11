#include "echo_app.hpp"

#include "foundation/log.hpp"

namespace atlas
{

EchoApp::EchoApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network)
{
}

auto EchoApp::init(int argc, char* argv[]) -> bool
{
    if (!ManagerApp::init(argc, argv))
        return false;

    ATLAS_LOG_INFO("EchoApp initialised (update_hertz={})", config().update_hertz);
    return true;
}

void EchoApp::on_tick_complete()
{
    ++tick_count_;

    // Log a heartbeat every 10 ticks so output is visible but not overwhelming
    if (tick_count_ % 10 == 0)
    {
        ATLAS_LOG_DEBUG("EchoApp tick={} uptime={:.1f}s", tick_count_, uptime_seconds());
    }
}

void EchoApp::fini()
{
    ATLAS_LOG_INFO("EchoApp shutting down after {} ticks", tick_count_);
    ManagerApp::fini();
}

}  // namespace atlas
