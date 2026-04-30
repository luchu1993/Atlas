// atlas_common is the precompiled-header source for
// atlas_foundation and downstream libs that REUSE_FROM it. Contents are
// the heaviest STL + project headers picked by per-TU include frequency
// across src/lib. Changing this file invalidates the PCH for every
// consumer, so keep it small and stable. Add only when a header is
// included in roughly half the consumer's TUs.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "foundation/clock.h"
#include "foundation/error.h"
#include "foundation/log.h"
