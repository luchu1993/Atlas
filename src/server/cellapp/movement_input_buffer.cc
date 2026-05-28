#include "movement_input_buffer.h"

#include <algorithm>
#include <limits>

namespace atlas {

auto MovementInputBuffer::Enqueue(EntityID entity_id, std::span<const movement::InputFrame> frames)
    -> EnqueueResult {
  EnqueueResult result;
  const auto dropped = static_cast<uint32_t>(
      std::min<std::size_t>(frames.size(), std::numeric_limits<uint32_t>::max()));
  if (entity_id == kInvalidEntityID) {
    result.dropped_invalid = dropped;
    return result;
  }
  if (max_frames_per_entity_ == 0) {
    result.dropped_overflow = dropped;
    return result;
  }

  auto& queue = queues_[entity_id];
  for (const auto& frame : frames) {
    if (!movement::IsInputFrameValid(frame)) {
      ++result.dropped_invalid;
      continue;
    }
    if (queue.has_last_accepted_seq &&
        !movement::IsInputSequenceNewer(frame.seq, queue.last_accepted_seq)) {
      ++result.dropped_stale;
      continue;
    }
    if (queue.has_last_accepted_seq &&
        movement::InputSequenceDelta(frame.seq, queue.last_accepted_seq) > max_sequence_gap_) {
      ++result.dropped_gap;
      continue;
    }
    if (queue.frames.size() >= max_frames_per_entity_) {
      queue.frames.pop_front();
      --total_queue_depth_;
      ++result.dropped_overflow;
    }
    queue.frames.push_back(frame);
    queue.last_accepted_seq = frame.seq;
    queue.has_last_accepted_seq = true;
    ++total_queue_depth_;
    ++result.accepted;
  }
  return result;
}

auto MovementInputBuffer::Drain(EntityID entity_id, uint32_t max_frames)
    -> std::vector<movement::InputFrame> {
  std::vector<movement::InputFrame> out;
  auto it = queues_.find(entity_id);
  if (it == queues_.end() || max_frames == 0) return out;

  auto& queue = it->second.frames;
  const auto count = std::min<std::size_t>(queue.size(), max_frames);
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(queue.front());
    queue.pop_front();
    --total_queue_depth_;
  }
  return out;
}

void MovementInputBuffer::Erase(EntityID entity_id) {
  auto it = queues_.find(entity_id);
  if (it == queues_.end()) return;
  total_queue_depth_ -= it->second.frames.size();
  queues_.erase(it);
}

void MovementInputBuffer::AppendEntityIdsWithPendingInput(std::vector<EntityID>& out) const {
  for (const auto& [entity_id, queue] : queues_) {
    if (!queue.frames.empty()) out.push_back(entity_id);
  }
}

auto MovementInputBuffer::QueueDepth(EntityID entity_id) const -> std::size_t {
  auto it = queues_.find(entity_id);
  return it == queues_.end() ? 0 : it->second.frames.size();
}

auto MovementInputBuffer::LastAcceptedSeq(EntityID entity_id) const -> uint32_t {
  auto it = queues_.find(entity_id);
  if (it == queues_.end() || !it->second.has_last_accepted_seq) return 0;
  return it->second.last_accepted_seq;
}

}  // namespace atlas
