#ifndef ATLAS_UE_CLIENT_CORE_ENTITY_ID_H_
#define ATLAS_UE_CLIENT_CORE_ENTITY_ID_H_

#include <cstdint>

namespace atlas {

using EntityId = uint32_t;
using EntityTypeId = uint16_t;

inline constexpr EntityId kInvalidEntityId = 0;

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_ENTITY_ID_H_
