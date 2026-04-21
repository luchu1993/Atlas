#include "client_app.h"
#include "foundation/runtime.h"

int main(int argc, char* argv[]) {
  // Install a ConsoleSink so ATLAS_LOG_* output (and therefore CLR bootstrap
  // errors surfaced by client_app / clrscript) reaches stderr. Without this,
  // atlas_client runs silently even when hostfxr / the script assembly fail
  // to load — a failure mode that hid Phase C2's script-client boot-up bugs
  // through much of its initial rollout.
  atlas::RuntimeConfig runtime_config;
  (void)atlas::Runtime::Initialize(runtime_config);

  const int rc = atlas::ClientApp::Run(argc, argv);

  atlas::Runtime::Finalize();
  return rc;
}
