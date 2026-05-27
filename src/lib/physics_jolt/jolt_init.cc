#include "physics_jolt/jolt_init.h"

namespace atlas::physics::jolt {

namespace {
// M1a stub: track state only. M1b will hold Factory / TempAllocator / JobSystem
// owned globals here behind the same Initialize / Shutdown surface.
bool g_initialized{false};
}

auto Initialize() -> bool {
  if (g_initialized) return true;
  g_initialized = true;
  return true;
}

void Shutdown() {
  g_initialized = false;
}

auto IsInitialized() -> bool {
  return g_initialized;
}

}  // namespace atlas::physics::jolt
