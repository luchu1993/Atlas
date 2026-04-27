#include "baseapp.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("baseapp");
  return atlas::BaseApp::Run(argc, argv);
}
