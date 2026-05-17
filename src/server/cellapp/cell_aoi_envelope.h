#ifndef ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_
#define ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_

#include <cstdint>

namespace atlas {

// Inner framing inside CellApp->Client delta messages; client dispatches
// on `kind`. Single-byte tag for forward-compat.
// Position-bearing kinds (Enter, PositionUpdate) carry an f64 server_time
// stamped from the cellapp's monotonic clock so AvatarFilter can interpolate.
enum class CellAoIEnvelopeKind : uint8_t {
  kEntityEnter = 1,           // type_id + pos/dir + on_ground + server_time + peer snapshot
  kEntityLeave = 2,           // empty
  kEntityPositionUpdate = 3,  // pos/dir/on_ground + server_time
  kEntityPropertyUpdate = 4,  // event_seq + audience-filtered delta bytes
  // Space-scoped (the u32 id-field is the space_id, not an entity id).
  kSpaceDataInit = 5,    // u32 count + [(u16 key, u32 vlen, vbytes)*]
  kSpaceDataUpdate = 6,  // u16 key + u32 vlen + vbytes
  kSpaceDataDelete = 7,  // u16 key
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_
