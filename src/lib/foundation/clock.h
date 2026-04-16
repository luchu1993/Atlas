#ifndef ATLAS_LIB_FOUNDATION_CLOCK_H_
#define ATLAS_LIB_FOUNDATION_CLOCK_H_

#include <chrono>
#include <cstdint>

namespace atlas {

// Central time types
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

using Seconds = std::chrono::duration<double>;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

// GameClock: controllable game time source
class GameClock {
 public:
  GameClock();

  // Advance by real elapsed time
  void Tick();

  // Advance by fixed delta (deterministic)
  void Tick(Duration delta);

  [[nodiscard]] auto Now() const -> TimePoint;
  [[nodiscard]] auto Elapsed() const -> Duration;
  [[nodiscard]] auto FrameDelta() const -> Duration;
  [[nodiscard]] auto FrameCount() const -> uint64_t;

  void SetTimeScale(double scale);
  [[nodiscard]] auto TimeScale() const -> double;

  void Reset();

 private:
  TimePoint real_start_;
  TimePoint real_last_tick_;
  Duration game_elapsed_{};
  Duration frame_delta_{};
  uint64_t frame_count_{0};
  double time_scale_{1.0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_CLOCK_H_
