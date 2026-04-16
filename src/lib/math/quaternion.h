#ifndef ATLAS_LIB_MATH_QUATERNION_H_
#define ATLAS_LIB_MATH_QUATERNION_H_

#include <cmath>

#include "math/vector3.h"

namespace atlas::math {

struct Matrix4;

struct Quaternion {
  float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f};

  constexpr Quaternion() = default;
  constexpr Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

  [[nodiscard]] static constexpr auto Identity() -> Quaternion { return {0, 0, 0, 1}; }
  [[nodiscard]] static auto FromAxisAngle(const Vector3& axis, float radians) -> Quaternion;
  [[nodiscard]] static auto FromEuler(float pitch, float yaw, float roll) -> Quaternion;
  [[nodiscard]] static auto FromMatrix(const Matrix4& m) -> Quaternion;

  [[nodiscard]] auto operator*(const Quaternion& q) const -> Quaternion;
  [[nodiscard]] constexpr auto Conjugate() const -> Quaternion { return {-x, -y, -z, w}; }
  [[nodiscard]] auto Inversed() const -> Quaternion;
  [[nodiscard]] auto Normalized() const -> Quaternion;
  void Normalize();

  [[nodiscard]] constexpr auto Dot(const Quaternion& q) const -> float {
    return x * q.x + y * q.y + z * q.z + w * q.w;
  }
  [[nodiscard]] auto Length() const -> float { return std::sqrt(Dot(*this)); }
  [[nodiscard]] constexpr auto LengthSquared() const -> float { return Dot(*this); }

  [[nodiscard]] auto Rotate(const Vector3& v) const -> Vector3;
  [[nodiscard]] auto ToMatrix() const -> Matrix4;

  constexpr auto operator==(const Quaternion& q) const -> bool {
    return x == q.x && y == q.y && z == q.z && w == q.w;
  }
};

[[nodiscard]] auto Slerp(const Quaternion& a, const Quaternion& b, float t) -> Quaternion;

}  // namespace atlas::math

#endif  // ATLAS_LIB_MATH_QUATERNION_H_
