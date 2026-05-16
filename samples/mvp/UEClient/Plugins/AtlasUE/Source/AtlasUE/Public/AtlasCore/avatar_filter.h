#ifndef ATLAS_UE_CLIENT_CORE_AVATAR_FILTER_H_
#define ATLAS_UE_CLIENT_CORE_AVATAR_FILTER_H_

#include <array>
#include <cstddef>
#include <functional>

#include "AtlasCore/core_export.h"
#include "AtlasCore/entity_view.h"

namespace atlas {

// Port of Atlas.Client.AvatarFilter. Same algorithm, same constants — see D11
// in docs/ue_client/decisions.md (no behavioural drift permitted).
class ATLAS_CORE_API AvatarFilter {
 public:
  static constexpr std::size_t kRingCapacity = 8;

  // wall_now is an injectable clock for tests; nullptr means steady_clock seconds.
  explicit AvatarFilter(std::function<double()> wall_now = nullptr);

  double latency_frames{3.0};
  double server_interval{0.1};
  double curve_power{2.0};
  double max_extrapolation{0.05};

  [[nodiscard]] std::size_t SampleCount() const { return count_; }
  [[nodiscard]] double CurrentLatency() const { return latency_current_; }
  [[nodiscard]] double TargetLatency() const { return latency_frames * server_interval; }

  void SnapLatencyToTarget() { latency_current_ = TargetLatency(); }
  void Reset();

  void Input(double server_time, const Vec3& pos, const Vec3& dir, bool on_ground);
  void UpdateLatency(double dt);
  bool TryEvaluate(Vec3& pos, Vec3& dir, bool& on_ground) const;

 private:
  struct Sample {
    double server_time{};
    Vec3 position{};
    Vec3 direction{};
    bool on_ground{false};
  };

  Vec3 ExtrapolatePosition(const Sample& s, double target_time) const;
  [[nodiscard]] std::size_t NewestIndex() const;
  [[nodiscard]] std::size_t OldestIndex() const;

  std::function<double()> wall_now_;
  std::array<Sample, kRingCapacity> ring_{};
  std::size_t write_index_{0};
  std::size_t count_{0};
  double latency_current_{0.0};
  double wall_from_server_offset_{0.0};
  bool initialised_{false};
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_AVATAR_FILTER_H_
