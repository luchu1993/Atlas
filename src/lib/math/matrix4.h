#ifndef ATLAS_LIB_MATH_MATRIX4_H_
#define ATLAS_LIB_MATH_MATRIX4_H_

#include <array>
#include <cmath>

#include "math/vector3.h"
#include "math/vector4.h"

namespace atlas::math {

struct Quaternion;

struct Matrix4 {
  // Row-major storage: m[row][col]
  // m[0..3] = row 0..3, each row has 4 floats
  std::array<float, 16> m{};

  constexpr Matrix4() = default;

  [[nodiscard]] constexpr auto operator()(int row, int col) const -> float {
    return m[static_cast<size_t>(row * 4 + col)];
  }
  [[nodiscard]] constexpr auto operator()(int row, int col) -> float& {
    return m[static_cast<size_t>(row * 4 + col)];
  }

  [[nodiscard]] static constexpr auto Identity() -> Matrix4 {
    Matrix4 result;
    result.m[0] = 1.0f;
    result.m[5] = 1.0f;
    result.m[10] = 1.0f;
    result.m[15] = 1.0f;
    return result;
  }

  [[nodiscard]] static constexpr auto Translation(const Vector3& t) -> Matrix4 {
    Matrix4 result = Identity();
    result.m[3] = t.x;
    result.m[7] = t.y;
    result.m[11] = t.z;
    return result;
  }

  [[nodiscard]] static constexpr auto Scale(const Vector3& s) -> Matrix4 {
    Matrix4 result;
    result.m[0] = s.x;
    result.m[5] = s.y;
    result.m[10] = s.z;
    result.m[15] = 1.0f;
    return result;
  }

  [[nodiscard]] static auto RotationX(float radians) -> Matrix4;
  [[nodiscard]] static auto RotationY(float radians) -> Matrix4;
  [[nodiscard]] static auto RotationZ(float radians) -> Matrix4;
  [[nodiscard]] static auto FromQuaternion(const Quaternion& q) -> Matrix4;
  [[nodiscard]] static auto LookAt(const Vector3& eye, const Vector3& target, const Vector3& up)
      -> Matrix4;

  [[nodiscard]] auto operator*(const Matrix4& other) const -> Matrix4;
  [[nodiscard]] auto TransformPoint(const Vector3& p) const -> Vector3;
  [[nodiscard]] auto TransformVector(const Vector3& v) const -> Vector3;

  [[nodiscard]] auto Transposed() const -> Matrix4;
  [[nodiscard]] auto Determinant() const -> float;
  [[nodiscard]] auto Inversed() const -> Matrix4;

  [[nodiscard]] constexpr auto GetTranslation() const -> Vector3 { return {m[3], m[7], m[11]}; }

  [[nodiscard]] auto GetScale() const -> Vector3;

  constexpr auto operator==(const Matrix4& other) const -> bool { return m == other.m; }
};

}  // namespace atlas::math

#endif  // ATLAS_LIB_MATH_MATRIX4_H_
