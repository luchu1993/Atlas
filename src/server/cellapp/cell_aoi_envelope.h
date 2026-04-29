#ifndef ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_
#define ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_

#include <cstdint>

namespace atlas {

// Inner framing inside CellApp→Client delta messages; client dispatches
// on `kind`. Single-byte tag for forward-compat.
enum class CellAoIEnvelopeKind : uint8_t {
  kEntityEnter = 1,           // peer entered AoI — payload: type_id + pos/dir + owner snapshot
  kEntityLeave = 2,           // peer left AoI — payload: empty
  kEntityPositionUpdate = 3,  // volatile position/direction — payload: pos/dir/on_ground
  kEntityPropertyUpdate = 4,  // ordered property delta — payload: audience-filtered delta bytes
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_
