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

// Controller serialise/deserialise for cross-process Offload migration.
// Wire v1: u8 count | per-controller (u8 kind | u32 id | i32 user_arg | payload).
//
// Caveats:
//   - ProximityController inside_peers is serialised as ids; non-local
//     peers drop silently (cross-CellApp partition is legal).
//   - Script-layer callbacks are NOT serialised — RestoreEntity must
//     reattach them on arrival.

// Returns nullptr for ids unknown on this CellApp.
using EntityLookupByIdFn = std::function<CellEntity*(uint32_t)>;

void SerializeControllersForMigration(const CellEntity& entity, BinaryWriter& w);

// Must run AFTER the entity is in entity_population_ (so lookup_peer
// works). Returns false on wire-format error; partial restore stays.
auto DeserializeControllersForMigration(CellEntity& entity, BinaryReader& r,
                                        const EntityLookupByIdFn& lookup_peer) -> bool;

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CONTROLLER_CODEC_H_
