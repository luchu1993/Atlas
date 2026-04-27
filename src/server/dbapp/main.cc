#include "dbapp.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("dbapp");
  return atlas::DBApp::Run(argc, argv);
}
