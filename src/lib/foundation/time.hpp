#pragma once

#include <chrono>
#include <cstdint>

namespace atlas
{

// Central time types
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

using Seconds      = std::chrono::duration<double>;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

// GameClock: controllable game time source
class GameClock
{
public:
    GameClock();

    // Advance by real elapsed time
    void tick();

    // Advance by fixed delta (deterministic)
    void tick(Duration delta);

    [[nodiscard]] auto now() const -> TimePoint;
    [[nodiscard]] auto elapsed() const -> Duration;
    [[nodiscard]] auto frame_delta() const -> Duration;
    [[nodiscard]] auto frame_count() const -> uint64_t;

    void set_time_scale(double scale);
    [[nodiscard]] auto time_scale() const -> double;

    void reset();

private:
    TimePoint real_start_;
    TimePoint real_last_tick_;
    Duration game_elapsed_{};
    Duration frame_delta_{};
    uint64_t frame_count_{0};
    double time_scale_{1.0};
};

} // namespace atlas
