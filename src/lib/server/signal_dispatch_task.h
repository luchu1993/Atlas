#ifndef ATLAS_LIB_SERVER_SIGNAL_DISPATCH_TASK_H_
#define ATLAS_LIB_SERVER_SIGNAL_DISPATCH_TASK_H_

#include "network/frequent_task.h"
#include "platform/signal_handler.h"

namespace atlas {

// FrequentTask adapter: dispatches pending OS signals on every EventDispatcher tick.
// Registered in ServerApp::init() so that signal callbacks run on the main thread.
class SignalDispatchTask : public FrequentTask {
 public:
  void DoTask() override { DispatchPendingSignals(); }
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SIGNAL_DISPATCH_TASK_H_
