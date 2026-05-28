#ifndef ATLAS_LIB_PHYSICS_JOLT_JOLT_INIT_H_
#define ATLAS_LIB_PHYSICS_JOLT_JOLT_INIT_H_

namespace atlas::physics::jolt {

// Wraps Jolt's required global init (Factory + RegisterTypes). Idempotent.
void Initialize();

// Tears down the globals set up by Initialize(). Safe to call without one.
void Shutdown();

[[nodiscard]] auto IsInitialized() -> bool;

}  // namespace atlas::physics::jolt

#endif  // ATLAS_LIB_PHYSICS_JOLT_JOLT_INIT_H_
