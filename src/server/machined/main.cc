#include "machined/machined_app.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("machined");
  return atlas::machined::MachinedApp::Run(argc, argv);
}
