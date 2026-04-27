// CellAppMgr process entry point.

#include "cellappmgr.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("cellappmgr");
  return atlas::CellAppMgr::Run(argc, argv);
}
