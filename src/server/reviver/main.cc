#include "platform/crash_handler.h"
#include "reviver.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("reviver");
  return atlas::Reviver::Run(argc, argv);
}
