#ifndef ATLAS_SERVER_CELLAPP_CELL_MOVEMENT_METRICS_H_
#define ATLAS_SERVER_CELLAPP_CELL_MOVEMENT_METRICS_H_

#include <chrono>
#include <cstdint>
#include <functional>

#include "foundation/latency_histogram.h"
#include "movement_input_buffer.h"
#include "movement_sim/movement_sim.h"

namespace atlas {

class WatcherRegistry;

class CellMovementMetrics {
 public:
  void RegisterWatchers(WatcherRegistry& wr,
                        std::function<std::size_t()> input_queue_depth,
                        std::function<std::size_t()> history_entity_count,
                        std::function<std::size_t()> history_sample_count,
                        std::function<std::size_t()> active_command_count);

  void RecordInputPacket();
  void RecordInputDrop(uint64_t count = 1);
  void RecordInputRateLimited();
  void RecordInputInvalidDrop();
  void RecordInputEnqueueResult(const MovementInputBuffer::EnqueueResult& result);
  void RecordFrameSimulated();
  void RecordPositionHistorySample();
  void RecordAckSent();
  void RecordCommandStarted();
  void RecordCommandEnded(movement::MovementCommandEndReason reason);
  void RecordStepTime(std::chrono::nanoseconds duration);

 private:
  LatencyHistogram step_time_;
  uint64_t input_packets_total_{0};
  uint64_t input_frames_enqueued_total_{0};
  uint64_t input_dropped_total_{0};
  uint64_t input_rate_limited_total_{0};
  uint64_t input_invalid_dropped_total_{0};
  uint64_t input_stale_dropped_total_{0};
  uint64_t input_seq_gap_dropped_total_{0};
  uint64_t input_overflow_dropped_total_{0};
  uint64_t frames_simulated_total_{0};
  uint64_t position_history_samples_recorded_total_{0};
  uint64_t ack_sent_total_{0};
  uint64_t command_started_total_{0};
  uint64_t command_ended_total_{0};
  uint64_t command_completed_total_{0};
  uint64_t command_cancelled_total_{0};
  uint64_t command_collision_total_{0};
  uint64_t command_invalid_total_{0};
};

}  // namespace atlas

#endif
