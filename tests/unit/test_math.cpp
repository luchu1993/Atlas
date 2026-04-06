#include <gtest/gtest.h>
#include "math/vector2.hpp"
#include "math/vector3.hpp"
#include "math/vector4.hpp"
#include "math/matrix4.hpp"
#include "math/quaternion.hpp"
#include "math/math_types.hpp"

#include <cmath>

using namespace atlas::math;

// ============================================================================
// Vector3
// ============================================================================

TEST(Vector3, Arithmetic)
{
    Vector3 a{1, 2, 3};
    Vector3 b{4, 5, 6};

    auto sum = a + b;
    EXPECT_EQ(sum.x, 5.0f);
    EXPECT_EQ(sum.y, 7.0f);
    EXPECT_EQ(sum.z, 9.0f);

    auto diff = b - a;
    EXPECT_EQ(diff.x, 3.0f);
    EXPECT_EQ(diff.y, 3.0f);
    EXPECT_EQ(diff.z, 3.0f);

    auto scaled = a * 2.0f;
    EXPECT_EQ(scaled.x, 2.0f);
    EXPECT_EQ(scaled.y, 4.0f);
    EXPECT_EQ(scaled.z, 6.0f);

    auto divided = b / 2.0f;
    EXPECT_NEAR(divided.x, 2.0f, 1e-5f);
    EXPECT_NEAR(divided.y, 2.5f, 1e-5f);
    EXPECT_NEAR(divided.z, 3.0f, 1e-5f);
}

TEST(Vector3, DotProduct)
{
    Vector3 a{1, 0, 0};
    Vector3 b{0, 1, 0};
    EXPECT_NEAR(a.dot(b), 0.0f, 1e-5f);

    Vector3 c{1, 2, 3};
    Vector3 d{4, 5, 6};
    EXPECT_NEAR(c.dot(d), 32.0f, 1e-5f);
}

TEST(Vector3, CrossProduct)
{
    auto c = Vector3::unit_x().cross(Vector3::unit_y());
    EXPECT_NEAR(c.x, 0.0f, 1e-5f);
    EXPECT_NEAR(c.y, 0.0f, 1e-5f);
    EXPECT_NEAR(c.z, 1.0f, 1e-5f);
}

TEST(Vector3, LengthAndNormalize)
{
    Vector3 v{3, 4, 0};
    EXPECT_NEAR(v.length(), 5.0f, 1e-5f);

    auto n = v.normalized();
    EXPECT_NEAR(n.length(), 1.0f, 1e-5f);
    EXPECT_NEAR(n.x, 0.6f, 1e-5f);
    EXPECT_NEAR(n.y, 0.8f, 1e-5f);
}

TEST(Vector3, ConstexprConstants)
{
    constexpr auto z = Vector3::zero();
    EXPECT_EQ(z.x, 0.0f);
    EXPECT_EQ(z.y, 0.0f);
    EXPECT_EQ(z.z, 0.0f);

    constexpr auto o = Vector3::one();
    EXPECT_EQ(o.x, 1.0f);
    EXPECT_EQ(o.y, 1.0f);
    EXPECT_EQ(o.z, 1.0f);

    constexpr auto ux = Vector3::unit_x();
    EXPECT_EQ(ux.x, 1.0f);
    EXPECT_EQ(ux.y, 0.0f);
    EXPECT_EQ(ux.z, 0.0f);
}

// ============================================================================
// Vector2
// ============================================================================

TEST(Vector2, ArithmeticDotLength)
{
    Vector2 a{3, 4};
    Vector2 b{1, 2};

    auto sum = a + b;
    EXPECT_NEAR(sum.x, 4.0f, 1e-5f);
    EXPECT_NEAR(sum.y, 6.0f, 1e-5f);

    EXPECT_NEAR(a.dot(b), 11.0f, 1e-5f);
    EXPECT_NEAR(a.length(), 5.0f, 1e-5f);
}

// ============================================================================
// Vector4
// ============================================================================

TEST(Vector4, ArithmeticDotLength)
{
    Vector4 a{1, 2, 3, 4};
    Vector4 b{5, 6, 7, 8};

    auto sum = a + b;
    EXPECT_NEAR(sum.x, 6.0f, 1e-5f);
    EXPECT_NEAR(sum.w, 12.0f, 1e-5f);

    EXPECT_NEAR(a.dot(b), 70.0f, 1e-5f);
    EXPECT_NEAR(a.length(), std::sqrt(30.0f), 1e-5f);
}

// ============================================================================
// Matrix4
// ============================================================================

TEST(Matrix4, Identity)
{
    auto id = Matrix4::identity();
    EXPECT_NEAR(id(0, 0), 1.0f, 1e-5f);
    EXPECT_NEAR(id(1, 1), 1.0f, 1e-5f);
    EXPECT_NEAR(id(2, 2), 1.0f, 1e-5f);
    EXPECT_NEAR(id(3, 3), 1.0f, 1e-5f);
    EXPECT_NEAR(id(0, 1), 0.0f, 1e-5f);
}

TEST(Matrix4, Translation)
{
    auto t = Matrix4::translation({10, 20, 30});
    EXPECT_NEAR(t.get_translation().x, 10.0f, 1e-5f);
    EXPECT_NEAR(t.get_translation().y, 20.0f, 1e-5f);
    EXPECT_NEAR(t.get_translation().z, 30.0f, 1e-5f);
}

TEST(Matrix4, Scale)
{
    auto s = Matrix4::scale({2, 3, 4});
    EXPECT_NEAR(s(0, 0), 2.0f, 1e-5f);
    EXPECT_NEAR(s(1, 1), 3.0f, 1e-5f);
    EXPECT_NEAR(s(2, 2), 4.0f, 1e-5f);
}

TEST(Matrix4, MultiplyIdentity)
{
    auto id = Matrix4::identity();
    auto t = Matrix4::translation({5, 6, 7});
    auto result = id * t;
    EXPECT_EQ(result, t);
}

TEST(Matrix4, InverseTimesOriginalIsIdentity)
{
    auto t = Matrix4::translation({3, 4, 5});
    auto inv = t.inversed();
    auto product = t * inv;
    auto id = Matrix4::identity();

    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            EXPECT_NEAR(product(i, j), id(i, j), 1e-5f);
        }
    }
}

TEST(Matrix4, TransformPoint)
{
    auto t = Matrix4::translation({10, 0, 0});
    auto p = t.transform_point({1, 2, 3});
    EXPECT_NEAR(p.x, 11.0f, 1e-5f);
    EXPECT_NEAR(p.y, 2.0f, 1e-5f);
    EXPECT_NEAR(p.z, 3.0f, 1e-5f);
}

TEST(Matrix4, TransformVector)
{
    auto t = Matrix4::translation({10, 0, 0});
    auto v = t.transform_vector({1, 2, 3});
    // Translation should not affect vectors
    EXPECT_NEAR(v.x, 1.0f, 1e-5f);
    EXPECT_NEAR(v.y, 2.0f, 1e-5f);
    EXPECT_NEAR(v.z, 3.0f, 1e-5f);
}

// ============================================================================
// Quaternion
// ============================================================================

TEST(Quaternion, Identity)
{
    auto q = Quaternion::identity();
    EXPECT_NEAR(q.x, 0.0f, 1e-5f);
    EXPECT_NEAR(q.y, 0.0f, 1e-5f);
    EXPECT_NEAR(q.z, 0.0f, 1e-5f);
    EXPECT_NEAR(q.w, 1.0f, 1e-5f);
}

TEST(Quaternion, FromAxisAngleRotateVector)
{
    // 90 degrees around Z should rotate (1,0,0) to (0,1,0)
    auto q = Quaternion::from_axis_angle(Vector3::unit_z(), kHalfPi);
    auto rotated = q.rotate(Vector3::unit_x());
    EXPECT_NEAR(rotated.x, 0.0f, 1e-5f);
    EXPECT_NEAR(rotated.y, 1.0f, 1e-5f);
    EXPECT_NEAR(rotated.z, 0.0f, 1e-5f);
}

TEST(Quaternion, SlerpEndpoints)
{
    auto a = Quaternion::identity();
    auto b = Quaternion::from_axis_angle(Vector3::unit_y(), kPi * 0.5f);

    auto at0 = slerp(a, b, 0.0f);
    EXPECT_NEAR(at0.x, a.x, 1e-5f);
    EXPECT_NEAR(at0.y, a.y, 1e-5f);
    EXPECT_NEAR(at0.z, a.z, 1e-5f);
    EXPECT_NEAR(at0.w, a.w, 1e-5f);

    auto at1 = slerp(a, b, 1.0f);
    EXPECT_NEAR(at1.x, b.x, 1e-5f);
    EXPECT_NEAR(at1.y, b.y, 1e-5f);
    EXPECT_NEAR(at1.z, b.z, 1e-5f);
    EXPECT_NEAR(at1.w, b.w, 1e-5f);
}

// ============================================================================
// Math constants
// ============================================================================

TEST(MathConstants, PiAndDegToRad)
{
    EXPECT_NEAR(kPi, 3.14159265f, 1e-5f);
    EXPECT_NEAR(deg_to_rad(180.0f), kPi, 1e-5f);
    EXPECT_NEAR(deg_to_rad(90.0f), kHalfPi, 1e-5f);
    EXPECT_NEAR(rad_to_deg(kPi), 180.0f, 1e-5f);
}

// ============================================================================
// Review issue: almost_equal() with negative differences
// ============================================================================

TEST(MathConstants, AlmostEqualPositiveDifference)
{
    EXPECT_TRUE(almost_equal(1.0f, 1.0f));
    EXPECT_TRUE(almost_equal(1.0f, 1.0f + kEpsilon * 0.5f));
    EXPECT_FALSE(almost_equal(1.0f, 1.0f + kEpsilon * 2.0f));
}

TEST(MathConstants, AlmostEqualNegativeDifference)
{
    // This was the bug: a < b case was not handled correctly
    EXPECT_TRUE(almost_equal(1.0f, 1.0f - kEpsilon * 0.5f));
    EXPECT_FALSE(almost_equal(1.0f, 1.0f - kEpsilon * 2.0f));
    EXPECT_TRUE(almost_equal(0.0f, -kEpsilon * 0.5f));
    EXPECT_FALSE(almost_equal(0.0f, -kEpsilon * 2.0f));
}

TEST(MathConstants, AlmostEqualSymmetric)
{
    // Must be symmetric: almost_equal(a, b) == almost_equal(b, a)
    float a = 1.0f;
    float b = 1.0f + kEpsilon * 0.5f;
    EXPECT_EQ(almost_equal(a, b), almost_equal(b, a));

    float c = 5.0f;
    float d = 5.0f + kEpsilon * 2.0f;
    EXPECT_EQ(almost_equal(c, d), almost_equal(d, c));
}

TEST(MathConstants, AlmostEqualCustomEpsilon)
{
    EXPECT_TRUE(almost_equal(1.0f, 1.1f, 0.2f));
    EXPECT_FALSE(almost_equal(1.0f, 1.3f, 0.2f));
}

// ============================================================================
// Review issue: Quaternion from zero axis
// ============================================================================

TEST(Quaternion, FromAxisAngleZeroAxisReturnsIdentity)
{
    auto q = Quaternion::from_axis_angle(Vector3::zero(), kHalfPi);
    EXPECT_NEAR(q.x, 0.0f, 1e-5f);
    EXPECT_NEAR(q.y, 0.0f, 1e-5f);
    EXPECT_NEAR(q.z, 0.0f, 1e-5f);
    EXPECT_NEAR(q.w, 1.0f, 1e-5f);
}

// ============================================================================
// Review issue: Quaternion from_matrix division by zero
// ============================================================================

TEST(Quaternion, FromMatrixIdentity)
{
    auto m = Matrix4::identity();
    auto q = Quaternion::from_matrix(m);
    EXPECT_NEAR(q.length(), 1.0f, 1e-5f);
    // Identity matrix should produce identity quaternion
    auto v = q.rotate(Vector3::unit_x());
    EXPECT_NEAR(v.x, 1.0f, 1e-5f);
    EXPECT_NEAR(v.y, 0.0f, 1e-5f);
    EXPECT_NEAR(v.z, 0.0f, 1e-5f);
}

TEST(Quaternion, FromMatrixRotationRoundTrip)
{
    // Create quaternion -> matrix -> quaternion, verify rotation is preserved
    auto q_orig = Quaternion::from_axis_angle(Vector3{1, 1, 0}.normalized(), deg_to_rad(60.0f));
    auto m = q_orig.to_matrix();
    auto q_back = Quaternion::from_matrix(m);

    // Verify they produce the same rotation (q and -q are equivalent)
    auto v_orig = q_orig.rotate(Vector3::unit_x());
    auto v_back = q_back.rotate(Vector3::unit_x());
    EXPECT_NEAR(v_orig.x, v_back.x, 1e-4f);
    EXPECT_NEAR(v_orig.y, v_back.y, 1e-4f);
    EXPECT_NEAR(v_orig.z, v_back.z, 1e-4f);
}

// ============================================================================
// Review issue: Matrix4 inverse of singular matrix
// ============================================================================

TEST(Matrix4, InverseSingularMatrixReturnsIdentity)
{
    // A zero matrix has determinant 0 — inverse should return identity
    Matrix4 zero_mat{};
    auto inv = zero_mat.inversed();
    auto id = Matrix4::identity();
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            EXPECT_NEAR(inv(i, j), id(i, j), 1e-5f);
        }
    }
}

TEST(Matrix4, InverseRotationRoundTrip)
{
    // Rotation matrix inverse = transpose for orthogonal matrices
    auto rot = Matrix4::rotation_z(deg_to_rad(45.0f));
    auto inv = rot.inversed();
    auto product = rot * inv;
    auto id = Matrix4::identity();
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            EXPECT_NEAR(product(i, j), id(i, j), 1e-5f);
        }
    }
}

TEST(Matrix4, InverseScaleRoundTrip)
{
    auto s = Matrix4::scale({2, 3, 4});
    auto inv = s.inversed();
    auto product = s * inv;
    auto id = Matrix4::identity();
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            EXPECT_NEAR(product(i, j), id(i, j), 1e-5f);
        }
    }
}

// ============================================================================
// Review issue: Vector normalize zero-length vector
// ============================================================================

TEST(Vector3, NormalizeZeroVector)
{
    auto n = Vector3::zero().normalized();
    EXPECT_NEAR(n.x, 0.0f, 1e-5f);
    EXPECT_NEAR(n.y, 0.0f, 1e-5f);
    EXPECT_NEAR(n.z, 0.0f, 1e-5f);
}

TEST(Vector2, NormalizeZeroVector)
{
    auto n = Vector2::zero().normalized();
    EXPECT_NEAR(n.x, 0.0f, 1e-5f);
    EXPECT_NEAR(n.y, 0.0f, 1e-5f);
}

// ============================================================================
// Review issue: Quaternion slerp at midpoint and with normalized inputs
// ============================================================================

TEST(Quaternion, SlerpMidpoint)
{
    auto a = Quaternion::identity();
    auto b = Quaternion::from_axis_angle(Vector3::unit_z(), kHalfPi);
    auto mid = slerp(a, b, 0.5f);

    // At t=0.5, rotation should be 45 degrees around Z
    auto v = mid.rotate(Vector3::unit_x());
    float expected_x = std::cos(kHalfPi * 0.5f);
    float expected_y = std::sin(kHalfPi * 0.5f);
    EXPECT_NEAR(v.x, expected_x, 1e-4f);
    EXPECT_NEAR(v.y, expected_y, 1e-4f);
    EXPECT_NEAR(v.z, 0.0f, 1e-4f);
}

TEST(Quaternion, SlerpNearlyParallel)
{
    // When quaternions are nearly identical, slerp should use nlerp fallback
    auto a = Quaternion::identity();
    auto b = Quaternion::from_axis_angle(Vector3::unit_y(), 1e-7f);
    auto result = slerp(a, b, 0.5f);
    EXPECT_NEAR(result.length(), 1.0f, 1e-4f);
}

// ============================================================================
// Review issue: float equality in Vector operator==
// ============================================================================

TEST(Vector3, EqualityExact)
{
    Vector3 a{1.0f, 2.0f, 3.0f};
    Vector3 b{1.0f, 2.0f, 3.0f};
    Vector3 c{1.0f, 2.0f, 3.001f};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}

TEST(Vector3, DistanceAndDistanceSquared)
{
    Vector3 a{1, 0, 0};
    Vector3 b{4, 0, 0};
    EXPECT_NEAR(a.distance(b), 3.0f, 1e-5f);
    EXPECT_NEAR(a.distance_squared(b), 9.0f, 1e-5f);
}
