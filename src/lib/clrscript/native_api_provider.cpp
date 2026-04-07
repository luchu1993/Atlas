#include "clrscript/native_api_provider.hpp"

#include <cassert>
#include <cstdio>

namespace atlas
{

static INativeApiProvider* g_provider = nullptr;

void set_native_api_provider(INativeApiProvider* provider)
{
    g_provider = provider;
}

INativeApiProvider& get_native_api_provider()
{
    // This is a programming error: set_native_api_provider() must be called
    // before ClrHost::initialize() and before any atlas_* function is invoked.
    assert(g_provider != nullptr &&
           "No INativeApiProvider registered. "
           "Call set_native_api_provider() before initialising ClrHost.");

    return *g_provider;
}

}  // namespace atlas
