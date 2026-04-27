#include "echo_app.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("echo");
  atlas::EventDispatcher dispatcher("echo");
  atlas::NetworkInterface network(dispatcher);
  atlas::EchoApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}
