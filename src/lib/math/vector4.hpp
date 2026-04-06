#pragma once

#include "math/math_types.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace atlas::math
{

struct Vector4
{
    float x{0.0f}, y{0.0f}, z{0.0f}, w{0.0f};

    constexpr Vector4() = default;
    constexpr Vector4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    constexpr auto operator+(const Vector4& v) const -> Vector4
    {
        return {x + v.x, y + v.y, z + v.z, w + v.w};
    }
    constexpr auto operator-(const Vector4& v) const -> Vector4
    {
        return {x - v.x, y - v.y, z - v.z, w - v.w};
    }
    constexpr auto operator*(float s) const -> Vector4 { return {x * s, y * s, z * s, w * s}; }
    constexpr auto operator/(float s) const -> Vector4 { return {x / s, y / s, z / s, w / s}; }
    constexpr auto operator-() const -> Vector4 { return {-x, -y, -z, -w}; }

    constexpr auto operator+=(const Vector4& v) -> Vector4&
    {
        x += v.x;
        y += v.y;
        z += v.z;
        w += v.w;
        return *this;
    }
    constexpr auto operator-=(const Vector4& v) -> Vector4&
    {
        x -= v.x;
        y -= v.y;
        z -= v.z;
        w -= v.w;
        return *this;
    }
    constexpr auto operator*=(float s) -> Vector4&
    {
        x *= s;
        y *= s;
        z *= s;
        w *= s;
        return *this;
    }
    constexpr auto operator/=(float s) -> Vector4&
    {
        x /= s;
        y /= s;
        z /= s;
        w /= s;
        return *this;
    }

    [[nodiscard]] constexpr auto dot(const Vector4& v) const -> float
    {
        return x * v.x + y * v.y + z * v.z + w * v.w;
    }
    [[nodiscard]] auto length() const -> float { return std::sqrt(x * x + y * y + z * z + w * w); }
    [[nodiscard]] constexpr auto length_squared() const -> float
    {
        return x * x + y * y + z * z + w * w;
    }
    [[nodiscard]] auto normalized() const -> Vector4
    {
        auto l = length();
        return l > kEpsilon ? Vector4{x / l, y / l, z / l, w / l} : Vector4{};
    }

    constexpr auto operator[](std::size_t i) -> float&
    {
        assert(i < 4);
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    constexpr auto operator[](std::size_t i) const -> float
    {
        assert(i < 4);
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }

    constexpr auto operator==(const Vector4& v) const -> bool
    {
        return x == v.x && y == v.y && z == v.z && w == v.w;
    }
    constexpr auto operator!=(const Vector4& v) const -> bool { return !(*this == v); }

    static constexpr auto zero() -> Vector4 { return {0, 0, 0, 0}; }
    static constexpr auto one() -> Vector4 { return {1, 1, 1, 1}; }
    static constexpr auto unit_x() -> Vector4 { return {1, 0, 0, 0}; }
    static constexpr auto unit_y() -> Vector4 { return {0, 1, 0, 0}; }
    static constexpr auto unit_z() -> Vector4 { return {0, 0, 1, 0}; }
    static constexpr auto unit_w() -> Vector4 { return {0, 0, 0, 1}; }
};

constexpr auto operator*(float s, const Vector4& v) -> Vector4
{
    return v * s;
}

}  // namespace atlas::math
