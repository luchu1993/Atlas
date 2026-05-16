#ifndef ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
#define ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

class ClientEntityManager;

// Mirrors src/server/cellapp/cell_aoi_envelope.h. M0 routes kEntityEnter,
// kEntityLeave, kEntityPositionUpdate; kEntityPropertyUpdate is recognised
// and silently dropped (delta sync arrives with codegen in M1).
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

// Decodes one envelope body — the bytes that follow the 0xF001 / 0xF003
// wire id. Returns kOk on success; truncated input or unknown kind returns
// the matching error and the manager is left untouched.
EnvelopeDecodeResult DecodeAoIEnvelope(const uint8_t* body, std::size_t len,
                                       ClientEntityManager& mgr);

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_AOI_ENVELOPE_H_
