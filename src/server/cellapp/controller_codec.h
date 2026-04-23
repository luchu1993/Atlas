#ifndef ATLAS_SERVER_CELLAPP_CONTROLLER_CODEC_H_
#define ATLAS_SERVER_CELLAPP_CONTROLLER_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "serialization/binary_stream.h"

namespace atlas {

class CellEntity;
class Controllers;

// ControllerCodec — serialize / deserialize an entity's Controllers for
// cross-process Offload migration.
//
// Wire format (version 1):
//   u8  count
//   for each controller:
//     u8   ControllerKind tag        // 1 = MoveToPoint, 2 = Timer, 3 = Proximity
//     u32  ControllerID               // preserved so C# handles survive
//     i32  user_arg                   // opaque passthrough from Add()
//     <per-kind payload>              // see individual encoders below
//
// The codec is a free-function pair (not a class) because it needs zero
// state — the owning CellEntity is passed in for every call and the
// live Controllers container is the output.
//
// Caveats:
//   • ProximityController's inside_peers set is serialised as peer
//     entity_ids (resolved through EntityRangeListNode::OwnerData ->
//     CellEntity::Id()). On arrival, each id is looked up in the
//     receiver's entity population; ids that don't resolve locally are
//     dropped silently — cross-CellApp partitions mean "not present
//     here" is a valid state. Scripts should not rely on OnLeave firing
//     for peers that live on a third CellApp at Offload time.
//
//   • Callbacks (TimerController::FireFn, ProximityController::EnterFn/
//     LeaveFn) are NOT serialised — they're script-layer closures. The
//     arrival CellApp must re-attach them from the C# entity's
//     RestoreEntity hook if the script wants them fired.

// Entity-id → CellEntity* lookup used by the codec on the receiving
// side to resolve ProximityController peer membership. Returns nullptr
// for ids that don't exist on this CellApp; those peers are dropped
// from the inside-peer seed (see the caveat above).
using EntityLookupByIdFn = std::function<CellEntity*(uint32_t)>;

// Serialise every live controller on `entity` into `w`. A no-op when
// entity has no Controllers. Does not mutate entity state.
void SerializeControllersForMigration(const CellEntity& entity, BinaryWriter& w);

// Rebuild controllers on `entity` from the blob. Must be called AFTER
// the CellEntity has been materialised and added to
// entity_population_, so `lookup_peer` can resolve ProximityController
// peer ids via the standard population index.
//
// Returns true on success (controllers installed), false on wire-
// format error. Partial restores are possible — an unknown Kind tag
// mid-stream aborts parsing and leaves any successfully-parsed
// controllers in place.
auto DeserializeControllersForMigration(CellEntity& entity, BinaryReader& r,
                                        const EntityLookupByIdFn& lookup_peer) -> bool;

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CONTROLLER_CODEC_H_
