#ifndef ATLAS_LIB_MATH_MATH_TYPES_H_
#define ATLAS_LIB_MATH_MATH_TYPES_H_

#include <algorithm>
#include <cmath>
#include <numbers>

namespace atlas::math {

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = kPi / 2.0f;
inline constexpr float kEpsilon = 1e-6f;
inline constexpr float kDegToRad = kPi / 180.0f;
inline constexpr float kRadToDeg = 180.0f / kPi;

[[nodiscard]] constexpr auto DegToRad(float deg) -> float {
  return deg * kDegToRad;
}

[[nodiscard]] constexpr auto RadToDeg(float rad) -> float {
  return rad * kRadToDeg;
}

template <typename T>
[[nodiscard]] constexpr auto Clamp(T val, T lo, T hi) -> T {
  return val < lo ? lo : (val > hi ? hi : val);
}

// Uses relative + absolute epsilon to handle both large and very small values.
[[nodiscard]] inline auto AlmostEqual(float a, float b, float rel_eps = 1e-5f,
                                      float abs_eps = kEpsilon) -> bool {
  float diff = std::abs(a - b);
  return diff <= std::max(std::abs(a), std::abs(b)) * rel_eps + abs_eps;
}

}  // namespace atlas::math

#endif  // ATLAS_LIB_MATH_MATH_TYPES_H_
