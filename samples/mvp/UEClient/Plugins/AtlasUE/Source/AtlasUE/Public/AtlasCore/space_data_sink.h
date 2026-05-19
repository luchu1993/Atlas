#ifndef ATLAS_UE_CLIENT_CORE_SPACE_DATA_SINK_H_
#define ATLAS_UE_CLIENT_CORE_SPACE_DATA_SINK_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

// Receives space-scoped key/value updates carried inside AoI envelope
// kinds 5/6/7. Payloads are raw little-endian bytes; the sink decides
// how to interpret each key (matches SpaceDataKeys.g.cs on the server).
class SpaceDataSink {
 public:
  virtual ~SpaceDataSink() = default;
  virtual void OnSpaceDataInit(uint32_t space_id, uint16_t key_id,
                               const uint8_t* data, std::size_t len) = 0;
  virtual void OnSpaceDataUpdate(uint32_t space_id, uint16_t key_id,
                                 const uint8_t* data, std::size_t len) = 0;
  virtual void OnSpaceDataDelete(uint32_t space_id, uint16_t key_id) = 0;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_SPACE_DATA_SINK_H_
