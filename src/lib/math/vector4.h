#ifndef ATLAS_LIB_MATH_VECTOR4_H_
#define ATLAS_LIB_MATH_VECTOR4_H_

#include <cassert>
#include <cmath>
#include <cstddef>

#include "math/math_types.h"

namespace atlas::math {

struct Vector4 {
  float x{0.0f}, y{0.0f}, z{0.0f}, w{0.0f};

  constexpr Vector4() = default;
  constexpr Vector4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

  constexpr auto operator+(const Vector4& v) const -> Vector4 {
    return {x + v.x, y + v.y, z + v.z, w + v.w};
  }
  constexpr auto operator-(const Vector4& v) const -> Vector4 {
    return {x - v.x, y - v.y, z - v.z, w - v.w};
  }
  constexpr auto operator*(float s) const -> Vector4 { return {x * s, y * s, z * s, w * s}; }
  constexpr auto operator/(float s) const -> Vector4 {
    assert(s != 0.0f && "Vector4 division by zero");
    return {x / s, y / s, z / s, w / s};
  }
  constexpr auto operator-() const -> Vector4 { return {-x, -y, -z, -w}; }

  constexpr auto operator+=(const Vector4& v) -> Vector4& {
    x += v.x;
    y += v.y;
    z += v.z;
    w += v.w;
    return *this;
  }
  constexpr auto operator-=(const Vector4& v) -> Vector4& {
    x -= v.x;
    y -= v.y;
    z -= v.z;
    w -= v.w;
    return *this;
  }
  constexpr auto operator*=(float s) -> Vector4& {
    x *= s;
    y *= s;
    z *= s;
    w *= s;
    return *this;
  }
  constexpr auto operator/=(float s) -> Vector4& {
    x /= s;
    y /= s;
    z /= s;
    w /= s;
    return *this;
  }

  [[nodiscard]] constexpr auto Dot(const Vector4& v) const -> float {
    return x * v.x + y * v.y + z * v.z + w * v.w;
  }
  [[nodiscard]] auto Length() const -> float { return std::sqrt(x * x + y * y + z * z + w * w); }
  [[nodiscard]] constexpr auto LengthSquared() const -> float {
    return x * x + y * y + z * z + w * w;
  }
  [[nodiscard]] auto Normalized() const -> Vector4 {
    auto l = Length();
    return l > kEpsilon ? Vector4{x / l, y / l, z / l, w / l} : Vector4{};
  }

  constexpr auto operator[](std::size_t i) -> float& {
    assert(i < 4);
    return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
  }
  constexpr auto operator[](std::size_t i) const -> float {
    assert(i < 4);
    return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
  }

  constexpr auto operator==(const Vector4& v) const -> bool {
    return x == v.x && y == v.y && z == v.z && w == v.w;
  }
  constexpr auto operator!=(const Vector4& v) const -> bool { return !(*this == v); }

  static constexpr auto Zero() -> Vector4 { return {0, 0, 0, 0}; }
  static constexpr auto One() -> Vector4 { return {1, 1, 1, 1}; }
  static constexpr auto UnitX() -> Vector4 { return {1, 0, 0, 0}; }
  static constexpr auto UnitY() -> Vector4 { return {0, 1, 0, 0}; }
  static constexpr auto UnitZ() -> Vector4 { return {0, 0, 1, 0}; }
  static constexpr auto UnitW() -> Vector4 { return {0, 0, 0, 1}; }
};

constexpr auto operator*(float s, const Vector4& v) -> Vector4 {
  return v * s;
}

}  // namespace atlas::math

#endif  // ATLAS_LIB_MATH_VECTOR4_H_
