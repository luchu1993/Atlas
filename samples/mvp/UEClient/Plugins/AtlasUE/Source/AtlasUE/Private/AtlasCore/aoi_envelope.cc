#include "AtlasCore/aoi_envelope.h"

#include <cmath>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/entity_view.h"
#include "AtlasCore/space_data_sink.h"
#include "AtlasCore/span_reader.h"

namespace atlas {

namespace {

// witness.cc::BuildEnterEnvelope: [u16 typeId][3f pos][3f dir][u8 onGround][f64
// serverTime][peerSnapshot].
constexpr std::size_t kEnterFixedBytes = 2 + 6 * 4 + 1 + 8;
// witness.cc::SendEntityUpdate volatile branch: [3f pos][3f dir][u8 onGround][f64 serverTime].
constexpr std::size_t kPositionUpdateBytes = 6 * 4 + 1 + 8;
constexpr std::size_t kPositionBatchPackedXZBytes = 3;
constexpr float kPositionBatchInvScale = 0.01f;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr uint8_t kPositionBatchHasTimeOffsets = 0x01;
constexpr uint8_t kPositionBatchHasOnGroundBits = 0x02;
constexpr uint8_t kPositionBatchAllOnGround = 0x04;
constexpr uint8_t kPositionBatchHasYOffsets = 0x08;
constexpr uint8_t kPositionBatchHasWideXZOffsets = 0x10;
constexpr uint8_t kPositionBatchSequentialEntityIds = 0x20;
constexpr uint8_t kPositionBatchKnownFlags =
    kPositionBatchHasTimeOffsets | kPositionBatchHasOnGroundBits | kPositionBatchAllOnGround |
    kPositionBatchHasYOffsets | kPositionBatchHasWideXZOffsets |
    kPositionBatchSequentialEntityIds;

int16_t DecodeSigned12(uint32_t value) {
  if ((value & 0x0800u) != 0) value |= 0xFFFFF000u;
  return static_cast<int16_t>(value);
}

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

EnvelopeDecodeResult DecodePositionUpdate(uint32_t id, SpanReader& r, ClientEntityManager& mgr) {
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

EnvelopeDecodeResult DecodePositionBatch(SpanReader& r, ClientEntityManager& mgr) {
  Vec3 origin;
  double base_time;
  uint16_t count;
  uint8_t flags;
  uint32_t id;
  if (!r.ReadVec3(origin) || !r.Read(base_time) || !r.Read(count) || !r.Read(flags) ||
      !r.ReadPackedUInt32(id)) {
    return EnvelopeDecodeResult::kTruncated;
  }
  if ((flags & static_cast<uint8_t>(~kPositionBatchKnownFlags)) != 0) {
    return EnvelopeDecodeResult::kUnsupportedFlags;
  }
  const bool has_time_offsets = (flags & kPositionBatchHasTimeOffsets) != 0;
  const bool has_on_ground_bits = (flags & kPositionBatchHasOnGroundBits) != 0;
  const bool has_y_offsets = (flags & kPositionBatchHasYOffsets) != 0;
  const bool has_wide_xz_offsets = (flags & kPositionBatchHasWideXZOffsets) != 0;
  const bool has_sequential_entity_ids = (flags & kPositionBatchSequentialEntityIds) != 0;
  const std::size_t on_ground_bytes = has_on_ground_bits ? (count + 7) / 8 : 0;
  if (!r.Has(on_ground_bytes)) return EnvelopeDecodeResult::kTruncated;
  const uint8_t* on_ground_bits = has_on_ground_bits ? r.Data() + r.Pos() : nullptr;
  r.Skip(on_ground_bytes);

  std::size_t min_entry_bytes = sizeof(uint8_t);
  if (!has_sequential_entity_ids) min_entry_bytes += 1;
  min_entry_bytes += has_wide_xz_offsets ? 2 * sizeof(int16_t) : kPositionBatchPackedXZBytes;
  if (has_y_offsets) min_entry_bytes += sizeof(int16_t);
  if (has_time_offsets) min_entry_bytes += sizeof(uint16_t);
  if (r.Remaining() < static_cast<std::size_t>(count) * min_entry_bytes) {
    return EnvelopeDecodeResult::kTruncated;
  }
  for (uint16_t i = 0; i < count; ++i) {
    uint32_t entity_delta = i == 0 ? 0 : 1;
    int16_t dx;
    int16_t dy = 0;
    int16_t dz;
    uint8_t yaw;
    uint16_t time_offset_ms = 0;
    if (!has_sequential_entity_ids && !r.ReadPackedUInt32(entity_delta)) {
      return EnvelopeDecodeResult::kTruncated;
    }
    if (has_wide_xz_offsets) {
      if (!r.Read(dx) || !r.Read(dz)) return EnvelopeDecodeResult::kTruncated;
    } else {
      uint8_t b0;
      uint8_t b1;
      uint8_t b2;
      if (!r.Read(b0) || !r.Read(b1) || !r.Read(b2)) return EnvelopeDecodeResult::kTruncated;
      const uint32_t packed = static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
                              (static_cast<uint32_t>(b2) << 16);
      dx = DecodeSigned12(packed & 0x0FFFu);
      dz = DecodeSigned12((packed >> 12) & 0x0FFFu);
    }
    if ((has_y_offsets && !r.Read(dy)) || !r.Read(yaw) ||
        (has_time_offsets && !r.Read(time_offset_ms))) {
      return EnvelopeDecodeResult::kTruncated;
    }
    id += entity_delta;
    const double radians = static_cast<double>(yaw) / 256.0 * kTwoPi;
    Vec3 pos{origin.x + static_cast<float>(dx) * kPositionBatchInvScale,
             origin.y + static_cast<float>(dy) * kPositionBatchInvScale,
             origin.z + static_cast<float>(dz) * kPositionBatchInvScale};
    Vec3 dir{static_cast<float>(std::sin(radians)), 0.0f, static_cast<float>(std::cos(radians))};
    const double server_time = base_time + static_cast<double>(time_offset_ms) * 0.001;
    const bool on_ground = has_on_ground_bits
                               ? (on_ground_bits[i / 8] & (uint8_t{1} << (i & 7))) != 0
                               : (flags & kPositionBatchAllOnGround) != 0;
    mgr.HandlePositionUpdate(id, server_time, pos, dir, on_ground);
  }
  return EnvelopeDecodeResult::kOk;
}

// witness.cc::BuildPropertyUpdateEnvelope: [u64 event_seq][sectionMask + delta].
EnvelopeDecodeResult DecodePropertyUpdate(uint32_t id, SpanReader& r, ClientEntityManager& mgr) {
  uint64_t event_seq;
  if (!r.Read(event_seq)) return EnvelopeDecodeResult::kTruncated;
  ClientEntity* entity = mgr.Find(id);
  if (entity == nullptr) return EnvelopeDecodeResult::kOk;  // left AoI
  return entity->ApplyDelta(r) ? EnvelopeDecodeResult::kOk : EnvelopeDecodeResult::kTruncated;
}

EnvelopeDecodeResult DecodeSpaceDataInit(uint32_t space_id, SpanReader& r, SpaceDataSink& sink) {
  uint32_t count;
  if (!r.Read(count)) return EnvelopeDecodeResult::kTruncated;
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t key;
    uint32_t vlen;
    if (!r.Read(key) || !r.Read(vlen)) return EnvelopeDecodeResult::kTruncated;
    if (!r.Has(vlen)) return EnvelopeDecodeResult::kTruncated;
    const uint8_t* bytes = vlen > 0 ? r.Data() + r.Pos() : nullptr;
    r.Skip(vlen);
    sink.OnSpaceDataInit(space_id, key, bytes, vlen);
  }
  return EnvelopeDecodeResult::kOk;
}

EnvelopeDecodeResult DecodeSpaceDataUpdate(uint32_t space_id, SpanReader& r, SpaceDataSink& sink) {
  uint16_t key;
  uint32_t vlen;
  if (!r.Read(key) || !r.Read(vlen)) return EnvelopeDecodeResult::kTruncated;
  if (!r.Has(vlen)) return EnvelopeDecodeResult::kTruncated;
  const uint8_t* bytes = vlen > 0 ? r.Data() + r.Pos() : nullptr;
  r.Skip(vlen);
  sink.OnSpaceDataUpdate(space_id, key, bytes, vlen);
  return EnvelopeDecodeResult::kOk;
}

EnvelopeDecodeResult DecodeSpaceDataDelete(uint32_t space_id, SpanReader& r, SpaceDataSink& sink) {
  uint16_t key;
  if (!r.Read(key)) return EnvelopeDecodeResult::kTruncated;
  sink.OnSpaceDataDelete(space_id, key);
  return EnvelopeDecodeResult::kOk;
}

}  // namespace

EnvelopeDecodeResult DecodeAoIEnvelope(const uint8_t* body, std::size_t len,
                                       ClientEntityManager& mgr, SpaceDataSink* space_data_sink) {
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
      if (space_data_sink == nullptr) return EnvelopeDecodeResult::kSpaceDataSkipped;
      return DecodeSpaceDataInit(entity_id, r, *space_data_sink);
    case EnvelopeKind::kSpaceDataUpdate:
      if (space_data_sink == nullptr) return EnvelopeDecodeResult::kSpaceDataSkipped;
      return DecodeSpaceDataUpdate(entity_id, r, *space_data_sink);
    case EnvelopeKind::kSpaceDataDelete:
      if (space_data_sink == nullptr) return EnvelopeDecodeResult::kSpaceDataSkipped;
      return DecodeSpaceDataDelete(entity_id, r, *space_data_sink);
    case EnvelopeKind::kEntityPositionBatch:
      return DecodePositionBatch(r, mgr);
    default:
      return EnvelopeDecodeResult::kUnknownKind;
  }
}

}  // namespace atlas
