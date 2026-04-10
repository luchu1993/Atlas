#include "clrscript/native_api_provider.hpp"

#include <atomic>
#include <cstdio>

namespace atlas
{

static std::atomic<INativeApiProvider*> g_provider{nullptr};

void set_native_api_provider(INativeApiProvider* provider)
{
    g_provider.store(provider, std::memory_order_release);
}

INativeApiProvider& get_native_api_provider()
{
    auto* p = g_provider.load(std::memory_order_acquire);
    // This is a fatal configuration error — set_native_api_provider() must be
    // called before ClrHost::initialize() and before any atlas_* function is
    // invoked.  Use hard abort (not assert) so the check survives Release builds.
    if (p == nullptr) [[unlikely]]
    {
        std::fprintf(stderr,
                     "FATAL: No INativeApiProvider registered. "
                     "Call set_native_api_provider() before initialising ClrHost.\n");
        std::abort();
    }
    return *p;
}

}  // namespace atlas
