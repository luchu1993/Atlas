#include "clrscript/native_api_provider.h"

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace atlas {
namespace {
std::atomic<INativeApiProvider*> g_provider{nullptr};
}  // namespace

void SetNativeApiProvider(INativeApiProvider* provider) {
  g_provider.store(provider, std::memory_order_release);
}

INativeApiProvider& GetNativeApiProvider() {
  auto* p = g_provider.load(std::memory_order_acquire);
  // Hard abort so the configuration check survives Release builds.
  if (p == nullptr) [[unlikely]] {
    std::cerr << "FATAL: No INativeApiProvider registered. "
              << "Call SetNativeApiProvider() before initialising ClrHost.\n";
    std::abort();
  }
  return *p;
}

}  // namespace atlas
