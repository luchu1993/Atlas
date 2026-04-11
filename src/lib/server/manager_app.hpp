#pragma once

#include "server/server_app.hpp"

namespace atlas
{

// ============================================================================
// ManagerApp — base class for manager / daemon processes with no script engine
//
// Used by: BaseAppMgr, CellAppMgr, DBAppMgr, Reviver, Machined.
// These processes only handle cluster-management messages; they never run C#.
// ============================================================================

class ManagerApp : public ServerApp
{
public:
    ManagerApp(EventDispatcher& dispatcher, NetworkInterface& network);

protected:
    // Opens a RUDP server on internal_port for all manager processes except machined.
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;

    void register_watchers() override;
};

}  // namespace atlas
