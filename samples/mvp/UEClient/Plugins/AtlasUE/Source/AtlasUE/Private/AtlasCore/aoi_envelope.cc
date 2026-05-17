#include "AtlasCore/aoi_envelope.h"

#include "AtlasCore/client_entity.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/entity_view.h"
#include "AtlasCore/span_reader.h"

namespace atlas {

namespace {

// witness.cc::BuildEnterEnvelope: [u16 typeId][3f pos][3f dir][u8 onGround][f64 serverTime][peerSnapshot].
constexpr std::size_t kEnterFixedBytes = 2 + 6 * 4 + 1 + 8;
// witness.cc::SendEntityUpdate volatile branch: [3f pos][3f dir][u8 onGround][f64 serverTime].
constexpr std::size_t kPositionUpdateBytes = 6 * 4 + 1 + 8;

EnvelopeDecodeResult DecodeEnter(uint32_t id, SpanReader& r, ClientEntityManager& mgr) {
  uint16_t type_id;
  Vec3 pos;
  Vec3 dir;
  uint8_t on_ground;
  double server_time;
  if (!r.Read(type_id) || !r.ReadVec3(pos) || !r.ReadVec3(dir) || !r.Read(on_ground) ||
      !r.Read(server_time)) {
    return EnvelopeDecodeResult::kTruncated;
  }
  // peerSnapshot bytes follow; ignored until codegen lands.
  mgr.HandleEnter(id, type_id, server_time, pos, dir, on_ground != 0);
  return EnvelopeDecodeResult::kOk;
}

EnvelopeDecodeResult DecodePositionUpdate(uint32_t id, SpanReader& r,
                                          ClientEntityManager& mgr) {
  Vec3 pos;
  Vec3 dir;
  uint8_t on_ground;
  double server_time;
  if (!r.ReadVec3(pos) || !r.ReadVec3(dir) || !r.Read(on_ground) || !r.Read(server_time)) {
    return EnvelopeDecodeResult::kTruncated;
  }
  mgr.HandlePositionUpdate(id, server_time, pos, dir, on_ground != 0);
  return EnvelopeDecodeResult::kOk;
}

// witness.cc::BuildPropertyUpdateEnvelope: [u64 event_seq][sectionMask + delta].
EnvelopeDecodeResult DecodePropertyUpdate(uint32_t id, SpanReader& r,
                                          ClientEntityManager& mgr) {
  uint64_t event_seq;
  if (!r.Read(event_seq)) return EnvelopeDecodeResult::kTruncated;
  ClientEntity* entity = mgr.Find(id);
  if (entity == nullptr) return EnvelopeDecodeResult::kOk;  // left AoI
  return entity->ApplyDelta(r) ? EnvelopeDecodeResult::kOk
                               : EnvelopeDecodeResult::kTruncated;
}

}  // namespace

EnvelopeDecodeResult DecodeAoIEnvelope(const uint8_t* body, std::size_t len,
                                       ClientEntityManager& mgr) {
  if (body == nullptr) return EnvelopeDecodeResult::kTruncated;
  SpanReader r(body, len);
  uint8_t kind;
  uint32_t entity_id;
  if (!r.Read(kind) || !r.Read(entity_id)) return EnvelopeDecodeResult::kTruncated;

  switch (static_cast<EnvelopeKind>(kind)) {
    case EnvelopeKind::kEntityEnter:
      return DecodeEnter(entity_id, r, mgr);
    case EnvelopeKind::kEntityLeave:
      mgr.HandleLeave(entity_id);
      return EnvelopeDecodeResult::kOk;
    case EnvelopeKind::kEntityPositionUpdate:
      return DecodePositionUpdate(entity_id, r, mgr);
    case EnvelopeKind::kEntityPropertyUpdate:
      return DecodePropertyUpdate(entity_id, r, mgr);
    case EnvelopeKind::kSpaceDataInit:
    case EnvelopeKind::kSpaceDataUpdate:
    case EnvelopeKind::kSpaceDataDelete:
      return EnvelopeDecodeResult::kSpaceDataSkipped;
    default:
      return EnvelopeDecodeResult::kUnknownKind;
  }
}

}  // namespace atlas
