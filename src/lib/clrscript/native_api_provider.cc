#include "clrscript/native_api_provider.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace atlas {

static std::atomic<INativeApiProvider*> g_provider{nullptr};

void SetNativeApiProvider(INativeApiProvider* provider) {
  g_provider.store(provider, std::memory_order_release);
}

INativeApiProvider& GetNativeApiProvider() {
  auto* p = g_provider.load(std::memory_order_acquire);
  // This is a fatal configuration error — SetNativeApiProvider() must be
  // called before ClrHost::Initialize() and before any Atlas* function is
  // invoked.  Use hard abort (not assert) so the check survives Release builds.
  if (p == nullptr) [[unlikely]] {
    std::fprintf(stderr,
                 "FATAL: No INativeApiProvider registered. "
                 "Call set_native_api_provider() before initialising ClrHost.\n");
    std::abort();
  }
  return *p;
}

}  // namespace atlas
