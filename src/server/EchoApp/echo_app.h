#ifndef ATLAS_SERVER_ECHOAPP_ECHO_APP_H_
#define ATLAS_SERVER_ECHOAPP_ECHO_APP_H_

#include <cstdint>

#include "server/manager_app.h"

namespace atlas {

class EchoApp : public ManagerApp {
 public:
  EchoApp(EventDispatcher& dispatcher, NetworkInterface& network);

 protected:
  auto Init(int argc, char* argv[]) -> bool override;
  void OnTickComplete() override;
  void Fini() override;

 private:
  uint64_t tick_count_{0};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_ECHOAPP_ECHO_APP_H_
