#ifndef ATLAS_LIB_PHYSICS_JOLT_JOLT_INIT_H_
#define ATLAS_LIB_PHYSICS_JOLT_JOLT_INIT_H_

namespace atlas::physics::jolt {

// Wraps Jolt's required global initialization (RegisterDefaultAllocator,
// Factory::sInstance, RegisterTypes). Idempotent — repeated calls are no-ops.
[[nodiscard]] auto Initialize() -> bool;

// Tears down the globals set up by Initialize(). Safe to call without
// a prior Initialize().
void Shutdown();

[[nodiscard]] auto IsInitialized() -> bool;

}  // namespace atlas::physics::jolt

#endif  // ATLAS_LIB_PHYSICS_JOLT_JOLT_INIT_H_
