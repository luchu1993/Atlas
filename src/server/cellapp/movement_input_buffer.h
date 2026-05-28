#ifndef ATLAS_SERVER_CELLAPP_MOVEMENT_INPUT_BUFFER_H_
#define ATLAS_SERVER_CELLAPP_MOVEMENT_INPUT_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

#include "movement_sim/movement_sim.h"
#include "server/entity_types.h"

namespace atlas {

class MovementInputBuffer {
 public:
  struct EnqueueResult {
    uint32_t accepted{0};
    uint32_t dropped_invalid{0};
    uint32_t dropped_stale{0};
    uint32_t dropped_gap{0};
    uint32_t dropped_overflow{0};
  };

  explicit MovementInputBuffer(std::size_t max_frames_per_entity = 32,
                               uint32_t max_sequence_gap = movement::kMaxInputSequenceGap)
      : max_frames_per_entity_(max_frames_per_entity), max_sequence_gap_(max_sequence_gap) {}

  [[nodiscard]] auto Enqueue(EntityID entity_id, std::span<const movement::InputFrame> frames)
      -> EnqueueResult;
  [[nodiscard]] auto Drain(EntityID entity_id, uint32_t max_frames)
      -> std::vector<movement::InputFrame>;
  void Erase(EntityID entity_id);
  void AppendEntityIdsWithPendingInput(std::vector<EntityID>& out) const;

  [[nodiscard]] auto QueueDepth(EntityID entity_id) const -> std::size_t;
  [[nodiscard]] auto TotalQueueDepth() const -> std::size_t { return total_queue_depth_; }
  [[nodiscard]] auto LastAcceptedSeq(EntityID entity_id) const -> uint32_t;

 private:
  struct EntityQueue {
    std::deque<movement::InputFrame> frames;
    uint32_t last_accepted_seq{0};
    bool has_last_accepted_seq{false};
  };

  std::unordered_map<EntityID, EntityQueue> queues_;
  std::size_t max_frames_per_entity_{32};
  uint32_t max_sequence_gap_{movement::kMaxInputSequenceGap};
  std::size_t total_queue_depth_{0};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_MOVEMENT_INPUT_BUFFER_H_
