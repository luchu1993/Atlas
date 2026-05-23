#include "AtlasCore/avatar_filter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace atlas {

namespace {
auto DefaultWallNow() -> double {
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  return std::chrono::duration<double>(clock::now() - start).count();
}
}  // namespace

AvatarFilter::AvatarFilter(std::function<double()> wall_now)
    : wall_now_(wall_now ? std::move(wall_now) : DefaultWallNow) {}

void AvatarFilter::Reset() {
  write_index_ = 0;
  count_ = 0;
  latency_current_ = 0.0;
  initialised_ = false;
}

void AvatarFilter::Input(double server_time, const Vec3& pos, const Vec3& dir, bool on_ground) {
  if (count_ > 0 && server_time <= ring_[NewestIndex()].server_time) return;

  ring_[write_index_] = {server_time, pos, dir, on_ground};
  write_index_ = (write_index_ + 1) % kRingCapacity;
  if (count_ < kRingCapacity) ++count_;

  const double offset = wall_now_() - server_time;
  if (!initialised_) {
    wall_from_server_offset_ = offset;
    latency_current_ = TargetLatency();
    initialised_ = true;
  } else {
    wall_from_server_offset_ = 0.95 * wall_from_server_offset_ + 0.05 * offset;
  }
}

void AvatarFilter::UpdateLatency(double dt) {
  if (!initialised_ || dt <= 0.0) return;
  const double target = TargetLatency();
  const double diff = target - latency_current_;
  const double abs_diff = std::abs(diff);
  const double speed = std::pow(abs_diff, curve_power) * 4.0;
  const double sign = (diff > 0.0) ? 1.0 : (diff < 0.0 ? -1.0 : 0.0);
  const double step = sign * std::min(abs_diff, speed * dt);
  latency_current_ += step;
}

bool AvatarFilter::TryEvaluate(Vec3& pos, Vec3& dir, bool& on_ground) const {
  pos = {};
  dir = {};
  on_ground = false;
  if (count_ == 0) return false;

  const double target_time = wall_now_() - wall_from_server_offset_ - latency_current_;
  const std::size_t newest = NewestIndex();

  if (count_ == 1 || target_time >= ring_[newest].server_time) {
    const auto& s = ring_[newest];
    pos = ExtrapolatePosition(s, target_time);
    dir = s.direction;
    on_ground = s.on_ground;
    return true;
  }

  const std::size_t oldest = OldestIndex();
  if (target_time <= ring_[oldest].server_time) {
    const auto& s = ring_[oldest];
    pos = s.position;
    dir = s.direction;
    on_ground = s.on_ground;
    return true;
  }

  std::size_t idx = oldest;
  for (std::size_t i = 0; i + 1 < count_; ++i) {
    const std::size_t next = (idx + 1) % kRingCapacity;
    const auto& a = ring_[idx];
    const auto& b = ring_[next];
    if (target_time >= a.server_time && target_time <= b.server_time) {
      const double span = b.server_time - a.server_time;
      const float t = span > 0.0 ? static_cast<float>((target_time - a.server_time) / span) : 0.0f;
      pos = {a.position.x + (b.position.x - a.position.x) * t,
             a.position.y + (b.position.y - a.position.y) * t,
             a.position.z + (b.position.z - a.position.z) * t};
      dir = {a.direction.x + (b.direction.x - a.direction.x) * t,
             a.direction.y + (b.direction.y - a.direction.y) * t,
             a.direction.z + (b.direction.z - a.direction.z) * t};
      on_ground = b.on_ground;
      return true;
    }
    idx = next;
  }

  const auto& fallback = ring_[newest];
  pos = fallback.position;
  dir = fallback.direction;
  on_ground = fallback.on_ground;
  return true;
}

Vec3 AvatarFilter::ExtrapolatePosition(const Sample& s, double target_time) const {
  double ahead = target_time - s.server_time;
  if (ahead <= 0.0 || count_ < 2) return s.position;
  if (ahead > max_extrapolation) return s.position;

  const std::size_t newest = NewestIndex();
  const std::size_t prev = (newest + kRingCapacity - 1) % kRingCapacity;
  const auto& a = ring_[prev];
  const auto& b = ring_[newest];
  const double span = b.server_time - a.server_time;
  if (span <= 0.0) return s.position;

  const float scale = static_cast<float>(ahead / span);
  return {b.position.x + (b.position.x - a.position.x) * scale,
          b.position.y + (b.position.y - a.position.y) * scale,
          b.position.z + (b.position.z - a.position.z) * scale};
}

std::size_t AvatarFilter::NewestIndex() const {
  return (write_index_ + kRingCapacity - 1) % kRingCapacity;
}

std::size_t AvatarFilter::OldestIndex() const {
  return count_ < kRingCapacity ? 0 : write_index_;
}

}  // namespace atlas
