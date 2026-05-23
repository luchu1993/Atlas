#ifndef ATLAS_LIB_PROTOCOL_AOI_ENVELOPE_H_
#define ATLAS_LIB_PROTOCOL_AOI_ENVELOPE_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

enum class CellAoIEnvelopeKind : uint8_t {
  kEntityEnter = 1,
  kEntityLeave = 2,
  kEntityPositionUpdate = 3,
  kEntityPropertyUpdate = 4,
  kSpaceDataInit = 5,
  kSpaceDataUpdate = 6,
  kSpaceDataDelete = 7,
  kEntityPositionBatch = 8,
};

inline constexpr std::size_t kPositionBatchPackedXZBytes = 3;
inline constexpr float kPositionBatchScale = 100.0f;
inline constexpr float kPositionBatchInvScale = 0.01f;
inline constexpr uint8_t kPositionBatchHasTimeOffsets = 0x01;
inline constexpr uint8_t kPositionBatchHasOnGroundBits = 0x02;
inline constexpr uint8_t kPositionBatchAllOnGround = 0x04;
inline constexpr uint8_t kPositionBatchHasYOffsets = 0x08;
inline constexpr uint8_t kPositionBatchHasWideXZOffsets = 0x10;
inline constexpr uint8_t kPositionBatchSequentialEntityIds = 0x20;
inline constexpr uint8_t kPositionBatchKnownFlags =
    kPositionBatchHasTimeOffsets | kPositionBatchHasOnGroundBits |
    kPositionBatchAllOnGround | kPositionBatchHasYOffsets |
    kPositionBatchHasWideXZOffsets | kPositionBatchSequentialEntityIds;
inline constexpr int16_t kPositionBatchMinPackedXZOffset = -2048;
inline constexpr int16_t kPositionBatchMaxPackedXZOffset = 2047;

}  // namespace atlas

#endif  // ATLAS_LIB_PROTOCOL_AOI_ENVELOPE_H_
