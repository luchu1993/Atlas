#ifndef ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
#define ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

class ClientEntityManager;

// Mirrors src/server/cellapp/cell_aoi_envelope.h. kEntityPropertyUpdate is
// recognised but skipped here — delta sync requires codegen.
enum class EnvelopeKind : uint8_t {
  kEntityEnter = 1,
  kEntityLeave = 2,
  kEntityPositionUpdate = 3,
  kEntityPropertyUpdate = 4,
};

enum class EnvelopeDecodeResult {
  kOk,
  kTruncated,
  kUnknownKind,
  kPropertyUpdateSkipped,
};

// On error the manager is left untouched.
EnvelopeDecodeResult DecodeAoIEnvelope(const uint8_t* body, std::size_t len,
                                       ClientEntityManager& mgr);

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
