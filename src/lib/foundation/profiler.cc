#include "foundation/profiler.h"

namespace atlas {

auto ProfilerEnabled() noexcept -> bool {
#if ATLAS_PROFILE_ENABLED
  return true;
#else
  return false;
#endif
}

}  // namespace atlas
