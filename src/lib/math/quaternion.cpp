#include "math/quaternion.hpp"
#include "math/matrix4.hpp"

namespace atlas::math
{

auto Quaternion::from_axis_angle(const Vector3& axis, float radians) -> Quaternion
{
    Vector3 n = axis.normalized();
    float half = radians * 0.5f;
    float s = std::sin(half);
    float c = std::cos(half);
    return {n.x * s, n.y * s, n.z * s, c};
}

auto Quaternion::from_euler(float pitch, float yaw, float roll) -> Quaternion
{
    float hp = pitch * 0.5f;
    float hy = yaw * 0.5f;
    float hr = roll * 0.5f;

    float sp = std::sin(hp);
    float cp = std::cos(hp);
    float sy = std::sin(hy);
    float cy = std::cos(hy);
    float sr = std::sin(hr);
    float cr = std::cos(hr);

    return {
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        sr * cp * cy - cr * sp * sy,
        cr * cp * cy + sr * sp * sy
    };
}

auto Quaternion::from_matrix(const Matrix4& mat) -> Quaternion
{
    // Shepperd's method
    float m00 = mat(0, 0), m11 = mat(1, 1), m22 = mat(2, 2);
    float trace = m00 + m11 + m22;

    Quaternion q;

    if (trace > 0.0f)
    {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        float inv_s = 1.0f / s;
        q.w = 0.25f * s;
        q.x = (mat(2, 1) - mat(1, 2)) * inv_s;
        q.y = (mat(0, 2) - mat(2, 0)) * inv_s;
        q.z = (mat(1, 0) - mat(0, 1)) * inv_s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        float inv_s = 1.0f / s;
        q.w = (mat(2, 1) - mat(1, 2)) * inv_s;
        q.x = 0.25f * s;
        q.y = (mat(0, 1) + mat(1, 0)) * inv_s;
        q.z = (mat(0, 2) + mat(2, 0)) * inv_s;
    }
    else if (m11 > m22)
    {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        float inv_s = 1.0f / s;
        q.w = (mat(0, 2) - mat(2, 0)) * inv_s;
        q.x = (mat(0, 1) + mat(1, 0)) * inv_s;
        q.y = 0.25f * s;
        q.z = (mat(1, 2) + mat(2, 1)) * inv_s;
    }
    else
    {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        float inv_s = 1.0f / s;
        q.w = (mat(1, 0) - mat(0, 1)) * inv_s;
        q.x = (mat(0, 2) + mat(2, 0)) * inv_s;
        q.y = (mat(1, 2) + mat(2, 1)) * inv_s;
        q.z = 0.25f * s;
    }

    return q;
}

auto Quaternion::operator*(const Quaternion& q) const -> Quaternion
{
    return {
        w * q.x + x * q.w + y * q.z - z * q.y,
        w * q.y - x * q.z + y * q.w + z * q.x,
        w * q.z + x * q.y - y * q.x + z * q.w,
        w * q.w - x * q.x - y * q.y - z * q.z
    };
}

auto Quaternion::inversed() const -> Quaternion
{
    float len_sq = length_squared();
    if (len_sq < kEpsilon)
    {
        return identity();
    }
    float inv = 1.0f / len_sq;
    return {-x * inv, -y * inv, -z * inv, w * inv};
}

auto Quaternion::normalized() const -> Quaternion
{
    float l = length();
    if (l < kEpsilon)
    {
        return identity();
    }
    float inv = 1.0f / l;
    return {x * inv, y * inv, z * inv, w * inv};
}

void Quaternion::normalize()
{
    float l = length();
    if (l < kEpsilon)
    {
        *this = identity();
        return;
    }
    float inv = 1.0f / l;
    x *= inv;
    y *= inv;
    z *= inv;
    w *= inv;
}

auto Quaternion::rotate(const Vector3& v) const -> Vector3
{
    // Optimized formula: v' = v + 2w * (q_xyz x v) + 2 * (q_xyz x (q_xyz x v))
    Vector3 q_xyz{x, y, z};
    Vector3 t = 2.0f * q_xyz.cross(v);
    return v + w * t + q_xyz.cross(t);
}

auto Quaternion::to_matrix() const -> Matrix4
{
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    Matrix4 result;

    result(0, 0) = 1.0f - 2.0f * (yy + zz);
    result(0, 1) = 2.0f * (xy - wz);
    result(0, 2) = 2.0f * (xz + wy);
    result(0, 3) = 0.0f;

    result(1, 0) = 2.0f * (xy + wz);
    result(1, 1) = 1.0f - 2.0f * (xx + zz);
    result(1, 2) = 2.0f * (yz - wx);
    result(1, 3) = 0.0f;

    result(2, 0) = 2.0f * (xz - wy);
    result(2, 1) = 2.0f * (yz + wx);
    result(2, 2) = 1.0f - 2.0f * (xx + yy);
    result(2, 3) = 0.0f;

    result(3, 0) = 0.0f;
    result(3, 1) = 0.0f;
    result(3, 2) = 0.0f;
    result(3, 3) = 1.0f;

    return result;
}

auto slerp(const Quaternion& a, const Quaternion& b, float t) -> Quaternion
{
    float cos_theta = a.dot(b);

    // Handle shortest path
    Quaternion b_adj = b;
    if (cos_theta < 0.0f)
    {
        b_adj = {-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }

    // If very close, use normalized linear interpolation
    if (cos_theta > 0.9999f)
    {
        Quaternion result{
            a.x + t * (b_adj.x - a.x),
            a.y + t * (b_adj.y - a.y),
            a.z + t * (b_adj.z - a.z),
            a.w + t * (b_adj.w - a.w)
        };
        return result.normalized();
    }

    float theta = std::acos(cos_theta);
    float sin_theta = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / sin_theta;
    float wb = std::sin(t * theta) / sin_theta;

    return Quaternion{
        wa * a.x + wb * b_adj.x,
        wa * a.y + wb * b_adj.y,
        wa * a.z + wb * b_adj.z,
        wa * a.w + wb * b_adj.w
    };
}

} // namespace atlas::math
