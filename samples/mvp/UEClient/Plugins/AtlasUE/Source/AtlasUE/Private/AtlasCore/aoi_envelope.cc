#include "AtlasCore/aoi_envelope.h"

#include <cstring>

#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/entity_view.h"

namespace atlas {

namespace {

// Little-endian readers; Atlas wire is fixed LE and all current targets are LE.
class SpanReader {
 public:
  SpanReader(const uint8_t* data, std::size_t len) : data_(data), len_(len) {}

  [[nodiscard]] bool Has(std::size_t n) const { return pos_ + n <= len_; }

  template <typename T>
  bool Read(T& out) {
    if (!Has(sizeof(T))) return false;
    std::memcpy(&out, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  bool ReadVec3(Vec3& v) { return Read(v.x) && Read(v.y) && Read(v.z); }

 private:
  const uint8_t* data_;
  std::size_t len_;
  std::size_t pos_{0};
};

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
      return EnvelopeDecodeResult::kPropertyUpdateSkipped;
    default:
      return EnvelopeDecodeResult::kUnknownKind;
  }
}

}  // namespace atlas
