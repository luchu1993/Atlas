// IL2CPP feasibility probe — see docs/spike_il2cpp_callback.md.

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

PROBE_API void probe_set_callback(ProbeCallback cb) {
  g_callback = cb;
}

PROBE_API void probe_fire(int32_t value) {
  if (g_callback) g_callback(value);
}
