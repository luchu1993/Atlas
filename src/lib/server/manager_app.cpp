#include "server/manager_app.hpp"

#include "foundation/log.hpp"
#include "network/network_interface.hpp"

namespace atlas
{

ManagerApp::ManagerApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ServerApp(dispatcher, network)
{
}

auto ManagerApp::init(int argc, char* argv[]) -> bool
{
    if (!ServerApp::init(argc, argv))
        return false;

    const auto& cfg = config();

    // Machined manages its own TCP server; all other manager processes open a
    // RUDP server on the internal port so peer services can connect via RUDP.
    if (cfg.process_type != ProcessType::Machined && cfg.internal_port > 0)
    {
        Address listen_addr(0, cfg.internal_port);
        if (auto r = network().start_rudp_server(listen_addr,
                                                 NetworkInterface::cluster_rudp_profile());
            !r)
        {
            ATLAS_LOG_ERROR("ManagerApp: failed to start RUDP server on port {}: {}",
                            cfg.internal_port, r.error().message());
            return false;
        }
        ATLAS_LOG_INFO("ManagerApp({}): RUDP server on {}", cfg.process_name,
                       network().rudp_address().to_string());
    }

    return true;
}

void ManagerApp::register_watchers()
{
    ServerApp::register_watchers();
    // Manager-specific watchers added here in concrete subclasses.
}

}  // namespace atlas
