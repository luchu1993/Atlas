#pragma once

#include "math/vector3.hpp"
#include "math/vector4.hpp"

#include <array>
#include <cmath>

namespace atlas::math
{

struct Quaternion;

struct Matrix4
{
    // Row-major storage: m[row][col]
    // m[0..3] = row 0..3, each row has 4 floats
    std::array<float, 16> m{};

    constexpr Matrix4() = default;

    [[nodiscard]] constexpr auto operator()(int row, int col) const -> float
    {
        return m[row * 4 + col];
    }
    [[nodiscard]] constexpr auto operator()(int row, int col) -> float&
    {
        return m[row * 4 + col];
    }

    [[nodiscard]] static constexpr auto identity() -> Matrix4
    {
        Matrix4 result;
        result.m[0]  = 1.0f;
        result.m[5]  = 1.0f;
        result.m[10] = 1.0f;
        result.m[15] = 1.0f;
        return result;
    }

    [[nodiscard]] static constexpr auto translation(const Vector3& t) -> Matrix4
    {
        Matrix4 result = identity();
        result.m[3]  = t.x;
        result.m[7]  = t.y;
        result.m[11] = t.z;
        return result;
    }

    [[nodiscard]] static constexpr auto scale(const Vector3& s) -> Matrix4
    {
        Matrix4 result;
        result.m[0]  = s.x;
        result.m[5]  = s.y;
        result.m[10] = s.z;
        result.m[15] = 1.0f;
        return result;
    }

    [[nodiscard]] static auto rotation_x(float radians) -> Matrix4;
    [[nodiscard]] static auto rotation_y(float radians) -> Matrix4;
    [[nodiscard]] static auto rotation_z(float radians) -> Matrix4;
    [[nodiscard]] static auto from_quaternion(const Quaternion& q) -> Matrix4;
    [[nodiscard]] static auto look_at(const Vector3& eye, const Vector3& target, const Vector3& up) -> Matrix4;

    [[nodiscard]] auto operator*(const Matrix4& other) const -> Matrix4;
    [[nodiscard]] auto transform_point(const Vector3& p) const -> Vector3;
    [[nodiscard]] auto transform_vector(const Vector3& v) const -> Vector3;

    [[nodiscard]] auto transposed() const -> Matrix4;
    [[nodiscard]] auto determinant() const -> float;
    [[nodiscard]] auto inversed() const -> Matrix4;

    [[nodiscard]] constexpr auto get_translation() const -> Vector3
    {
        return {m[3], m[7], m[11]};
    }

    [[nodiscard]] auto get_scale() const -> Vector3;

    constexpr auto operator==(const Matrix4& other) const -> bool { return m == other.m; }
};

} // namespace atlas::math
