#include "cell_movement_metrics.h"

#include <utility>

#include "server/watcher.h"

namespace atlas {

void CellMovementMetrics::RegisterWatchers(
    WatcherRegistry& wr, std::function<std::size_t()> input_queue_depth,
    std::function<std::size_t()> history_entity_count,
    std::function<std::size_t()> history_sample_count,
    std::function<std::size_t()> active_command_count) {
  wr.Add<uint64_t>("movement/input_packets_total", input_packets_total_);
  wr.Add<uint64_t>("movement/input_frames_enqueued_total", input_frames_enqueued_total_);
  wr.Add<uint64_t>("movement/input_dropped_total", input_dropped_total_);
  wr.Add<uint64_t>("movement/input_rate_limited_total", input_rate_limited_total_);
  wr.Add<uint64_t>("movement/input_invalid_dropped_total", input_invalid_dropped_total_);
  wr.Add<uint64_t>("movement/input_stale_dropped_total", input_stale_dropped_total_);
  wr.Add<uint64_t>("movement/input_seq_gap_dropped_total", input_seq_gap_dropped_total_);
  wr.Add<uint64_t>("movement/input_overflow_dropped_total", input_overflow_dropped_total_);
  wr.Add<std::size_t>("movement/input_queue_depth", std::move(input_queue_depth));
  wr.Add<std::size_t>("movement/position_history_entities", std::move(history_entity_count));
  wr.Add<std::size_t>("movement/position_history_samples", std::move(history_sample_count));
  wr.Add<uint64_t>("movement/position_history_samples_recorded_total",
                   position_history_samples_recorded_total_);
  wr.Add<uint64_t>("movement/frames_simulated_total", frames_simulated_total_);
  wr.Add<uint64_t>("movement/ack_sent_total", ack_sent_total_);
  wr.Add<std::size_t>("movement/active_commands", std::move(active_command_count));
  wr.Add<uint64_t>("movement/command_started_total", command_started_total_);
  wr.Add<uint64_t>("movement/command_ended_total", command_ended_total_);
  wr.Add<uint64_t>("movement/command_completed_total", command_completed_total_);
  wr.Add<uint64_t>("movement/command_cancelled_total", command_cancelled_total_);
  wr.Add<uint64_t>("movement/command_collision_total", command_collision_total_);
  wr.Add<uint64_t>("movement/command_invalid_total", command_invalid_total_);
  RegisterLatencyWatchers(wr, "movement/step_time", step_time_);
  wr.Add<double>("movement/step_time_us_p95",
                 std::function<double()>(
                     [this] { return step_time_.QuantileMicros(0.95); }));
}

void CellMovementMetrics::RecordInputPacket() {
  ++input_packets_total_;
}

void CellMovementMetrics::RecordInputDrop(uint64_t count) {
  input_dropped_total_ += count;
}

void CellMovementMetrics::RecordInputRateLimited() {
  ++input_rate_limited_total_;
}

void CellMovementMetrics::RecordInputInvalidDrop() {
  ++input_invalid_dropped_total_;
  ++input_dropped_total_;
}

void CellMovementMetrics::RecordInputEnqueueResult(
    const MovementInputBuffer::EnqueueResult& result) {
  input_frames_enqueued_total_ += result.accepted;
  input_invalid_dropped_total_ += result.dropped_invalid;
  input_stale_dropped_total_ += result.dropped_stale;
  input_seq_gap_dropped_total_ += result.dropped_gap;
  input_overflow_dropped_total_ += result.dropped_overflow;
  input_dropped_total_ += result.dropped_invalid + result.dropped_stale +
                          result.dropped_gap + result.dropped_overflow;
}

void CellMovementMetrics::RecordFrameSimulated() {
  ++frames_simulated_total_;
}

void CellMovementMetrics::RecordPositionHistorySample() {
  ++position_history_samples_recorded_total_;
}

void CellMovementMetrics::RecordAckSent() {
  ++ack_sent_total_;
}

void CellMovementMetrics::RecordCommandStarted() {
  ++command_started_total_;
}

void CellMovementMetrics::RecordCommandEnded(movement::MovementCommandEndReason reason) {
  ++command_ended_total_;
  switch (reason) {
    case movement::MovementCommandEndReason::kCompleted:
      ++command_completed_total_;
      break;
    case movement::MovementCommandEndReason::kCancelled:
      ++command_cancelled_total_;
      break;
    case movement::MovementCommandEndReason::kCollision:
      ++command_collision_total_;
      break;
    case movement::MovementCommandEndReason::kInvalid:
      ++command_invalid_total_;
      break;
  }
}

void CellMovementMetrics::RecordStepTime(std::chrono::nanoseconds duration) {
  step_time_.Record(duration);
}

}  // namespace atlas
