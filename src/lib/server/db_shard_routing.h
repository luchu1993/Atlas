#ifndef ATLAS_LIB_SERVER_DB_SHARD_ROUTING_H_
#define ATLAS_LIB_SERVER_DB_SHARD_ROUTING_H_

#include <cstdint>
#include <limits>
#include <string_view>

#include "server/entity_types.h"

namespace atlas {

[[nodiscard]] inline auto DbShardRouteKey(std::string_view identifier) -> DatabaseID {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : identifier) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  const uint64_t kMax = static_cast<uint64_t>(std::numeric_limits<DatabaseID>::max());
  return static_cast<DatabaseID>(1 + (hash % (kMax - 1)));
}

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_DB_SHARD_ROUTING_H_
