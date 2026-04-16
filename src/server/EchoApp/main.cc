#include "echo_app.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"

int main(int argc, char* argv[]) {
  atlas::EventDispatcher dispatcher("echo");
  atlas::NetworkInterface network(dispatcher);
  atlas::EchoApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}
