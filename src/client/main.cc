#include "client_app.h"
#include "foundation/runtime.h"
#include "platform/crash_handler.h"

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("client");

  // Runtime must come up before ClientApp::Run so ATLAS_LOG_* reach stderr
  // during hostfxr / script-assembly bring-up.
  atlas::RuntimeConfig runtime_config;
  (void)atlas::Runtime::Initialize(runtime_config);

  const int rc = atlas::ClientApp::Run(argc, argv);

  atlas::Runtime::Finalize();
  return rc;
}
