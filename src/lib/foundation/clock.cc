#include "foundation/clock.h"

#include <algorithm>

namespace atlas {

GameClock::GameClock() : real_start_(Clock::now()), real_last_tick_(real_start_) {}

void GameClock::Tick() {
  auto real_now = Clock::now();
  auto real_delta = real_now - real_last_tick_;
  auto scaled =
      Duration(static_cast<Duration::rep>(static_cast<double>(real_delta.count()) * time_scale_));

  game_elapsed_ += scaled;
  frame_delta_ = scaled;
  ++frame_count_;
  real_last_tick_ = real_now;
}

void GameClock::Tick(Duration delta) {
  auto scaled =
      Duration(static_cast<Duration::rep>(static_cast<double>(delta.count()) * time_scale_));
  frame_delta_ = scaled;
  game_elapsed_ += scaled;
  ++frame_count_;
  real_last_tick_ = Clock::now();
}

auto GameClock::Now() const -> TimePoint {
  return real_start_ + game_elapsed_;
}

auto GameClock::Elapsed() const -> Duration {
  return game_elapsed_;
}

auto GameClock::FrameDelta() const -> Duration {
  return frame_delta_;
}

auto GameClock::FrameCount() const -> uint64_t {
  return frame_count_;
}

void GameClock::SetTimeScale(double scale) {
  time_scale_ = std::max(0.0, scale);
}

auto GameClock::TimeScale() const -> double {
  return time_scale_;
}

void GameClock::Reset() {
  real_start_ = Clock::now();
  real_last_tick_ = real_start_;
  game_elapsed_ = Duration{};
  frame_delta_ = Duration{};
  frame_count_ = 0;
  time_scale_ = 1.0;
}

}  // namespace atlas
