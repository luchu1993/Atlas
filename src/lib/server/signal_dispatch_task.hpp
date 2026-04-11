#pragma once

#include "network/frequent_task.hpp"
#include "platform/signal_handler.hpp"

namespace atlas
{

// FrequentTask adapter: dispatches pending OS signals on every EventDispatcher tick.
// Registered in ServerApp::init() so that signal callbacks run on the main thread.
class SignalDispatchTask : public FrequentTask
{
public:
    void do_task() override { dispatch_pending_signals(); }
};

}  // namespace atlas
