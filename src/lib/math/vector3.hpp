#pragma once

#include "math/math_types.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace atlas::math
{

struct Vector3
{
    float x{0.0f}, y{0.0f}, z{0.0f};

    constexpr Vector3() = default;
    constexpr Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    constexpr auto operator+(const Vector3& v) const -> Vector3
    {
        return {x + v.x, y + v.y, z + v.z};
    }
    constexpr auto operator-(const Vector3& v) const -> Vector3
    {
        return {x - v.x, y - v.y, z - v.z};
    }
    constexpr auto operator*(float s) const -> Vector3 { return {x * s, y * s, z * s}; }
    constexpr auto operator/(float s) const -> Vector3
    {
        assert(s != 0.0f && "Vector3 division by zero");
        return {x / s, y / s, z / s};
    }
    constexpr auto operator-() const -> Vector3 { return {-x, -y, -z}; }

    constexpr auto operator+=(const Vector3& v) -> Vector3&
    {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }
    constexpr auto operator-=(const Vector3& v) -> Vector3&
    {
        x -= v.x;
        y -= v.y;
        z -= v.z;
        return *this;
    }
    constexpr auto operator*=(float s) -> Vector3&
    {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    constexpr auto operator/=(float s) -> Vector3&
    {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }

    [[nodiscard]] constexpr auto dot(const Vector3& v) const -> float
    {
        return x * v.x + y * v.y + z * v.z;
    }

    [[nodiscard]] constexpr auto cross(const Vector3& v) const -> Vector3
    {
        return {y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x};
    }

    [[nodiscard]] auto length() const -> float { return std::sqrt(x * x + y * y + z * z); }
    [[nodiscard]] constexpr auto length_squared() const -> float { return x * x + y * y + z * z; }
    [[nodiscard]] auto normalized() const -> Vector3
    {
        auto l = length();
        return l > kEpsilon ? Vector3{x / l, y / l, z / l} : Vector3{};
    }

    [[nodiscard]] auto distance(const Vector3& v) const -> float { return (*this - v).length(); }
    [[nodiscard]] constexpr auto distance_squared(const Vector3& v) const -> float
    {
        return (*this - v).length_squared();
    }

    constexpr auto operator[](std::size_t i) -> float&
    {
        assert(i < 3);
        return i == 0 ? x : (i == 1 ? y : z);
    }
    constexpr auto operator[](std::size_t i) const -> float
    {
        assert(i < 3);
        return i == 0 ? x : (i == 1 ? y : z);
    }

    constexpr auto operator==(const Vector3& v) const -> bool
    {
        return x == v.x && y == v.y && z == v.z;
    }
    constexpr auto operator!=(const Vector3& v) const -> bool { return !(*this == v); }

    static constexpr auto zero() -> Vector3 { return {0, 0, 0}; }
    static constexpr auto one() -> Vector3 { return {1, 1, 1}; }
    static constexpr auto unit_x() -> Vector3 { return {1, 0, 0}; }
    static constexpr auto unit_y() -> Vector3 { return {0, 1, 0}; }
    static constexpr auto unit_z() -> Vector3 { return {0, 0, 1}; }
};

constexpr auto operator*(float s, const Vector3& v) -> Vector3
{
    return v * s;
}

}  // namespace atlas::math
