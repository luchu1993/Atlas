#include "loginapp.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("loginapp");
  return atlas::LoginApp::Run(argc, argv);
}
