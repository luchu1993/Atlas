#pragma once

#include "math/vector3.hpp"

#include <cmath>

namespace atlas::math
{

struct Matrix4;

struct Quaternion
{
    float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f};

    constexpr Quaternion() = default;
    constexpr Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    [[nodiscard]] static constexpr auto identity() -> Quaternion { return {0, 0, 0, 1}; }
    [[nodiscard]] static auto from_axis_angle(const Vector3& axis, float radians) -> Quaternion;
    [[nodiscard]] static auto from_euler(float pitch, float yaw, float roll) -> Quaternion;
    [[nodiscard]] static auto from_matrix(const Matrix4& m) -> Quaternion;

    [[nodiscard]] auto operator*(const Quaternion& q) const -> Quaternion;
    [[nodiscard]] constexpr auto conjugate() const -> Quaternion { return {-x, -y, -z, w}; }
    [[nodiscard]] auto inversed() const -> Quaternion;
    [[nodiscard]] auto normalized() const -> Quaternion;
    void normalize();

    [[nodiscard]] constexpr auto dot(const Quaternion& q) const -> float
    {
        return x * q.x + y * q.y + z * q.z + w * q.w;
    }
    [[nodiscard]] auto length() const -> float { return std::sqrt(dot(*this)); }
    [[nodiscard]] constexpr auto length_squared() const -> float { return dot(*this); }

    [[nodiscard]] auto rotate(const Vector3& v) const -> Vector3;
    [[nodiscard]] auto to_matrix() const -> Matrix4;

    constexpr auto operator==(const Quaternion& q) const -> bool
    {
        return x == q.x && y == q.y && z == q.z && w == q.w;
    }
};

[[nodiscard]] auto slerp(const Quaternion& a, const Quaternion& b, float t) -> Quaternion;

}  // namespace atlas::math
