#include "foundation/time.hpp"

#include <algorithm>

namespace atlas
{

GameClock::GameClock() : real_start_(Clock::now()), real_last_tick_(real_start_) {}

void GameClock::tick()
{
    auto real_now = Clock::now();
    auto real_delta = real_now - real_last_tick_;
    auto scaled =
        Duration(static_cast<Duration::rep>(static_cast<double>(real_delta.count()) * time_scale_));

    game_elapsed_ += scaled;
    frame_delta_ = scaled;
    ++frame_count_;
    real_last_tick_ = real_now;
}

void GameClock::tick(Duration delta)
{
    auto scaled =
        Duration(static_cast<Duration::rep>(static_cast<double>(delta.count()) * time_scale_));
    frame_delta_ = scaled;
    game_elapsed_ += scaled;
    ++frame_count_;
    real_last_tick_ = Clock::now();
}

auto GameClock::now() const -> TimePoint
{
    return real_start_ + game_elapsed_;
}

auto GameClock::elapsed() const -> Duration
{
    return game_elapsed_;
}

auto GameClock::frame_delta() const -> Duration
{
    return frame_delta_;
}

auto GameClock::frame_count() const -> uint64_t
{
    return frame_count_;
}

void GameClock::set_time_scale(double scale)
{
    time_scale_ = std::max(0.0, scale);
}

auto GameClock::time_scale() const -> double
{
    return time_scale_;
}

void GameClock::reset()
{
    real_start_ = Clock::now();
    real_last_tick_ = real_start_;
    game_elapsed_ = Duration{};
    frame_delta_ = Duration{};
    frame_count_ = 0;
    time_scale_ = 1.0;
}

}  // namespace atlas
