#include "client_app.h"
#include "foundation/runtime.h"

int main(int argc, char* argv[]) {
  // Initialize the runtime so ATLAS_LOG_* output reaches stderr — without
  // this, atlas_client runs silently on hostfxr / script-assembly load errors.
  atlas::RuntimeConfig runtime_config;
  (void)atlas::Runtime::Initialize(runtime_config);

  const int rc = atlas::ClientApp::Run(argc, argv);

  atlas::Runtime::Finalize();
  return rc;
}
