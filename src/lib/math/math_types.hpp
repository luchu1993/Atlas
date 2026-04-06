#pragma once

#include <cmath>
#include <numbers>

namespace atlas::math
{

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = kPi / 2.0f;
inline constexpr float kEpsilon = 1e-6f;
inline constexpr float kDegToRad = kPi / 180.0f;
inline constexpr float kRadToDeg = 180.0f / kPi;

[[nodiscard]] constexpr auto deg_to_rad(float deg) -> float
{
    return deg * kDegToRad;
}

[[nodiscard]] constexpr auto rad_to_deg(float rad) -> float
{
    return rad * kRadToDeg;
}

template <typename T>
[[nodiscard]] constexpr auto clamp(T val, T lo, T hi) -> T
{
    return val < lo ? lo : (val > hi ? hi : val);
}

[[nodiscard]] constexpr auto almost_equal(float a, float b, float epsilon = kEpsilon) -> bool
{
    return (a - b) < epsilon && (b - a) < epsilon;
}

} // namespace atlas::math
