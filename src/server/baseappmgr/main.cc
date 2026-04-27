#include "baseappmgr.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("baseappmgr");
  return atlas::BaseAppMgr::Run(argc, argv);
}
