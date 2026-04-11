#pragma once

#include "server/manager_app.hpp"

#include <cstdint>

namespace atlas
{

// ============================================================================
// EchoApp — minimal ManagerApp for framework verification
//
// Verifies:
//   • Process starts, tick fires at update_hertz, graceful shutdown on Ctrl+C
//   • Watcher queries (app/uptime_seconds, tick/total_count, etc.)
//   • Config loading from CLI / JSON
// ============================================================================

class EchoApp : public ManagerApp
{
public:
    EchoApp(EventDispatcher& dispatcher, NetworkInterface& network);

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void on_tick_complete() override;
    void fini() override;

private:
    uint64_t tick_count_{0};
};

}  // namespace atlas
