#ifndef ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
#define ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

class ClientEntityManager;

// Mirrors src/server/cellapp/cell_aoi_envelope.h. SpaceData kinds are
// recognised but skipped — UE MVP doesn't consume them yet. The u32
// id-field on kSpaceData* is space_id, not entity.
enum class EnvelopeKind : uint8_t {
  kEntityEnter = 1,
  kEntityLeave = 2,
  kEntityPositionUpdate = 3,
  kEntityPropertyUpdate = 4,
  kSpaceDataInit = 5,
  kSpaceDataUpdate = 6,
  kSpaceDataDelete = 7,
};

enum class EnvelopeDecodeResult {
  kOk,
  kTruncated,
  kUnknownKind,
  kSpaceDataSkipped,
};

// On error the manager is left untouched.
EnvelopeDecodeResult DecodeAoIEnvelope(const uint8_t* body, std::size_t len,
                                       ClientEntityManager& mgr);

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
