// CellApp process entry point.

#include "cellapp.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  // Install before app construction so an AV during init still produces a dump.
  atlas::InstallDefaultCrashHandler("cellapp");
  return atlas::CellApp::Run(argc, argv);
}
