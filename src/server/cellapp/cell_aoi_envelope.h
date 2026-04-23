#ifndef ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_
#define ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_

#include <cstdint>

namespace atlas {

// ============================================================================
// CellAoIEnvelope — inner framing used inside CellApp → Client delta messages
//
// CellApp's AoI downstream payloads ride on top of BaseApp's existing
// CellApp→Client relay messages (SelfRpcFromCell, ReplicatedReliableDeltaFromCell,
// ReplicatedDeltaFromCell — see delta_forwarder.h's three-path contract).
// Those outer messages carry opaque `payload` bytes; the CellAoIEnvelope is
// the shape of those bytes. The client decodes `kind` first and dispatches
// to the matching handler.
//
// The single-byte tag leaves plenty of room for future envelope kinds
// (terrain, vehicle, etc.) without a wire break.
// ============================================================================

enum class CellAoIEnvelopeKind : uint8_t {
  kEntityEnter = 1,           // peer entered AoI — payload: type_id + pos/dir + owner snapshot
  kEntityLeave = 2,           // peer left AoI — payload: empty
  kEntityPositionUpdate = 3,  // volatile position/direction — payload: pos/dir/on_ground
  kEntityPropertyUpdate = 4,  // ordered property delta — payload: audience-filtered delta bytes
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELL_AOI_ENVELOPE_H_
