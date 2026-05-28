#include "physics_jolt/jolt_init.h"

#include <cstdarg>
#include <cstdio>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

namespace atlas::physics::jolt {

namespace {

bool g_initialized{false};

void TraceImpl(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buffer[1024];
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[Jolt] %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool AssertFailedImpl(const char* expression, const char* message, const char* file,
                      JPH::uint line) {
  std::fprintf(stderr, "[Jolt assert] %s:%u (%s) %s\n", file, line, expression,
               message != nullptr ? message : "");
  return true;
}
#endif

}  // namespace

void Initialize() {
  if (g_initialized) return;

  JPH::RegisterDefaultAllocator();
  JPH::Trace = &TraceImpl;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = &AssertFailedImpl;
#endif
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();

  g_initialized = true;
}

void Shutdown() {
  if (!g_initialized) return;

  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;

  g_initialized = false;
}

auto IsInitialized() -> bool {
  return g_initialized;
}

}  // namespace atlas::physics::jolt
