#pragma once

#include "math/math_types.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace atlas::math
{

struct Vector2
{
    float x{0.0f}, y{0.0f};

    constexpr Vector2() = default;
    constexpr Vector2(float x, float y) : x(x), y(y) {}

    constexpr auto operator+(const Vector2& v) const -> Vector2 { return {x + v.x, y + v.y}; }
    constexpr auto operator-(const Vector2& v) const -> Vector2 { return {x - v.x, y - v.y}; }
    constexpr auto operator*(float s) const -> Vector2 { return {x * s, y * s}; }
    constexpr auto operator/(float s) const -> Vector2
    {
        assert(s != 0.0f && "Vector2 division by zero");
        return {x / s, y / s};
    }
    constexpr auto operator-() const -> Vector2 { return {-x, -y}; }

    constexpr auto operator+=(const Vector2& v) -> Vector2&
    {
        x += v.x;
        y += v.y;
        return *this;
    }
    constexpr auto operator-=(const Vector2& v) -> Vector2&
    {
        x -= v.x;
        y -= v.y;
        return *this;
    }
    constexpr auto operator*=(float s) -> Vector2&
    {
        x *= s;
        y *= s;
        return *this;
    }
    constexpr auto operator/=(float s) -> Vector2&
    {
        x /= s;
        y /= s;
        return *this;
    }

    [[nodiscard]] constexpr auto dot(const Vector2& v) const -> float { return x * v.x + y * v.y; }
    [[nodiscard]] auto length() const -> float { return std::sqrt(x * x + y * y); }
    [[nodiscard]] constexpr auto length_squared() const -> float { return x * x + y * y; }
    [[nodiscard]] auto normalized() const -> Vector2
    {
        auto l = length();
        return l > kEpsilon ? Vector2{x / l, y / l} : Vector2{};
    }

    constexpr auto operator[](std::size_t i) -> float&
    {
        assert(i < 2);
        return i == 0 ? x : y;
    }
    constexpr auto operator[](std::size_t i) const -> float
    {
        assert(i < 2);
        return i == 0 ? x : y;
    }

    constexpr auto operator==(const Vector2& v) const -> bool { return x == v.x && y == v.y; }
    constexpr auto operator!=(const Vector2& v) const -> bool { return !(*this == v); }

    static constexpr auto zero() -> Vector2 { return {0, 0}; }
    static constexpr auto one() -> Vector2 { return {1, 1}; }
};

constexpr auto operator*(float s, const Vector2& v) -> Vector2
{
    return v * s;
}

}  // namespace atlas::math
