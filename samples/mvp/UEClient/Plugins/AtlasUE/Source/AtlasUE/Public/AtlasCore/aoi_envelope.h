#ifndef ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
#define ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

class ClientEntityManager;
class SpaceDataSink;

// Mirrors src/lib/protocol/aoi_envelope.h. SpaceData kinds are skipped
// unless a sink is supplied; the kSpaceData* id field is space_id.
enum class EnvelopeKind : uint8_t {
  kEntityEnter = 1,
  kEntityLeave = 2,
  kEntityPositionUpdate = 3,
  kEntityPropertyUpdate = 4,
  kSpaceDataInit = 5,
  kSpaceDataUpdate = 6,
  kSpaceDataDelete = 7,
  kEntityPositionBatch = 8,
};

enum class EnvelopeDecodeResult {
  kOk,
  kTruncated,
  kUnknownKind,
  kUnsupportedFlags,
  kSpaceDataSkipped,
};

// On error the manager and sink are left untouched. Pass nullptr to drop
// SpaceData envelopes; the decoder returns kSpaceDataSkipped.
EnvelopeDecodeResult DecodeAoIEnvelope(const uint8_t* body, std::size_t len,
                                       ClientEntityManager& mgr,
                                       SpaceDataSink* space_data_sink = nullptr);

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
