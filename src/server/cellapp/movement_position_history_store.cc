#include "movement_position_history_store.h"

#include <algorithm>
#include <iterator>

#include "movement_sim/movement_sim.h"

namespace atlas {
namespace {

auto Lerp(const math::Vector3& a, const math::Vector3& b, float t) -> math::Vector3 {
  return a * (1.0f - t) + b * t;
}

auto Interpolate(const MovementPositionSample& before, const MovementPositionSample& after,
                 uint32_t server_tick) -> MovementPositionSample {
  const float span = static_cast<float>(after.server_tick - before.server_tick);
  const float t = span > 0.0f ? static_cast<float>(server_tick - before.server_tick) / span : 0.0f;

  MovementPositionSample sample;
  sample.server_tick = server_tick;
  sample.state = before.state;
  sample.state.position = Lerp(before.state.position, after.state.position, t);
  sample.state.velocity = Lerp(before.state.velocity, after.state.velocity, t);
  const auto direction = Lerp(before.state.direction, after.state.direction, t);
  sample.state.direction =
      direction.LengthSquared() > 0.0f ? direction.Normalized() : after.state.direction;
  if (t >= 0.5f) {
    sample.state.flags = after.state.flags;
    sample.state.last_processed_input_seq = after.state.last_processed_input_seq;
  }
  return sample;
}

}  // namespace

MovementPositionHistoryStore::MovementPositionHistoryStore(
    std::size_t max_samples_per_entity)
    : max_samples_per_entity_(max_samples_per_entity) {}

void MovementPositionHistoryStore::Record(EntityID entity_id, uint32_t server_tick,
                                          const movement::MovementState& state) {
  if (entity_id == kInvalidEntityID || max_samples_per_entity_ == 0) return;

  auto& history = histories_[entity_id];
  if (!history.empty()) {
    const auto back_tick = history.back().server_tick;
    if (server_tick == back_tick) {
      history.back() = MovementPositionSample{server_tick, state};
      return;
    }
    if (!movement::IsInputSequenceNewer(server_tick, back_tick)) return;
  }

  history.push_back(MovementPositionSample{server_tick, state});
  while (history.size() > max_samples_per_entity_) history.pop_front();
}

auto MovementPositionHistoryStore::Find(EntityID entity_id) const
    -> const std::deque<MovementPositionSample>* {
  auto it = histories_.find(entity_id);
  return it == histories_.end() ? nullptr : &it->second;
}

auto MovementPositionHistoryStore::SampleAt(EntityID entity_id, uint32_t server_tick) const
    -> std::optional<MovementPositionSample> {
  const auto* history = Find(entity_id);
  if (history == nullptr || history->empty()) return std::nullopt;
  if (server_tick < history->front().server_tick ||
      server_tick > history->back().server_tick) {
    return std::nullopt;
  }

  auto it = std::lower_bound(history->begin(), history->end(), server_tick,
                             [](const MovementPositionSample& sample, uint32_t tick) {
                               return sample.server_tick < tick;
                             });
  if (it == history->end()) return std::nullopt;
  if (it->server_tick == server_tick) return *it;
  if (it == history->begin()) return std::nullopt;
  return Interpolate(*std::prev(it), *it, server_tick);
}

void MovementPositionHistoryStore::Erase(EntityID entity_id) {
  histories_.erase(entity_id);
}

auto MovementPositionHistoryStore::TotalSampleCount() const -> std::size_t {
  std::size_t total = 0;
  for (const auto& [_, history] : histories_) total += history.size();
  return total;
}

}  // namespace atlas
