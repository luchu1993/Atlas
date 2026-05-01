// Minimal native callback probe for the Phase 0 IL2CPP feasibility spike
// (see docs/client/UNITY_NATIVE_DLL_DESIGN.md §10 Phase 0).
//
// Exports two symbols:
//   probe_set_callback(cb)  — caller registers a function pointer
//   probe_fire(value)       — DLL invokes the registered callback with `value`
//
// The Unity test project calls these via P/Invoke and checks both
// callback-registration patterns (`[UnmanagedCallersOnly]` function pointer
// and `[MonoPInvokeCallback]` delegate) on every Unity build target
// (Editor Mono, Standalone Win IL2CPP, Android arm64 IL2CPP, iOS arm64
// static + `__Internal`). If both Mono and IL2CPP fire the callback we
// pick pattern A for the real DLL; if only Mono fires we drop to B.
//
// Library is intentionally header-less and dependency-less: the spike
// must isolate the FFI layer, not Atlas's code.

#include <cstdint>

#if defined(_WIN32)
#define PROBE_API extern "C" __declspec(dllexport)
#else
#define PROBE_API extern "C" __attribute__((visibility("default")))
#endif

namespace {
using ProbeCallback = void (*)(int32_t);
ProbeCallback g_callback = nullptr;
}  // namespace

PROBE_API void probe_set_callback(ProbeCallback cb) { g_callback = cb; }

PROBE_API void probe_fire(int32_t value) {
  if (g_callback) g_callback(value);
}
