#include "echo_app.hpp"
#include "network/event_dispatcher.hpp"
#include "network/network_interface.hpp"

int main(int argc, char* argv[])
{
    atlas::EventDispatcher dispatcher("echo");
    atlas::NetworkInterface network(dispatcher);
    atlas::EchoApp app(dispatcher, network);
    return app.run_app(argc, argv);
}
