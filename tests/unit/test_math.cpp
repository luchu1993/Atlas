#include "math/math_types.hpp"
#include "math/matrix4.hpp"
#include "math/quaternion.hpp"
#include "math/vector2.hpp"
#include "math/vector3.hpp"
#include "math/vector4.hpp"

#include <gtest/gtest.h>

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
    // For values near 1.0, tolerance ≈ rel_eps(1e-5) * 1.0 + abs_eps(1e-6) ≈ 1.1e-5.
    // A difference of 1e-7 is well within tolerance; a difference of 1e-3 is clearly outside.
    EXPECT_TRUE(almost_equal(1.0f, 1.0f));
    EXPECT_TRUE(almost_equal(1.0f, 1.0f + 1e-7f));
    EXPECT_FALSE(almost_equal(1.0f, 1.0f + 1e-3f));
}

TEST(MathConstants, AlmostEqualNegativeDifference)
{
    // Symmetric with the positive case.
    EXPECT_TRUE(almost_equal(1.0f, 1.0f - 1e-7f));
    EXPECT_FALSE(almost_equal(1.0f, 1.0f - 1e-3f));
    // Near zero: only abs_eps governs, so kEpsilon * 0.5 (5e-7) passes, 1e-3 fails.
    EXPECT_TRUE(almost_equal(0.0f, 5e-7f));
    EXPECT_FALSE(almost_equal(0.0f, 1e-3f));
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

// ============================================================================
// Matrix4::get_scale  (row-major correctness regression)
// ============================================================================

TEST(Matrix4, GetScaleUniform)
{
    auto m = Matrix4::scale({3.0f, 3.0f, 3.0f});
    auto s = m.get_scale();
    EXPECT_NEAR(s.x, 3.0f, 1e-5f);
    EXPECT_NEAR(s.y, 3.0f, 1e-5f);
    EXPECT_NEAR(s.z, 3.0f, 1e-5f);
}

TEST(Matrix4, GetScaleNonUniform)
{
    auto m = Matrix4::scale({2.0f, 3.0f, 5.0f});
    auto s = m.get_scale();
    EXPECT_NEAR(s.x, 2.0f, 1e-5f);
    EXPECT_NEAR(s.y, 3.0f, 1e-5f);
    EXPECT_NEAR(s.z, 5.0f, 1e-5f);
}

TEST(Matrix4, GetScaleFromTRS)
{
    // Translation * Scale: scale must survive the composition
    auto t = Matrix4::translation({10.0f, 20.0f, 30.0f});
    auto sc = Matrix4::scale({4.0f, 4.0f, 4.0f});
    auto trs = t * sc;
    auto extracted = trs.get_scale();
    EXPECT_NEAR(extracted.x, 4.0f, 1e-5f);
    EXPECT_NEAR(extracted.y, 4.0f, 1e-5f);
    EXPECT_NEAR(extracted.z, 4.0f, 1e-5f);
}

// ============================================================================
// Quaternion::slerp  (cos_theta clamp — no NaN on identical quaternions)
// ============================================================================

TEST(Quaternion, SlerpIdenticalNonNaN)
{
    // Identical unit quaternions → dot = 1.0 exactly (or > 1.0 due to FP).
    // Before the clamp fix, acos(>1) returned NaN.
    auto q = Quaternion::from_axis_angle({0, 1, 0}, 1.0f);
    auto r = slerp(q, q, 0.5f);

    EXPECT_FALSE(std::isnan(r.x));
    EXPECT_FALSE(std::isnan(r.y));
    EXPECT_FALSE(std::isnan(r.z));
    EXPECT_FALSE(std::isnan(r.w));
    // Result should equal the input quaternion
    EXPECT_NEAR(r.x, q.x, 1e-5f);
    EXPECT_NEAR(r.y, q.y, 1e-5f);
    EXPECT_NEAR(r.z, q.z, 1e-5f);
    EXPECT_NEAR(r.w, q.w, 1e-5f);
}

TEST(Quaternion, SlerpHalfwayIsNormalized)
{
    auto a = Quaternion::identity();
    auto b = Quaternion::from_axis_angle({0, 1, 0}, 1.5708f);  // ~90 deg
    auto mid = slerp(a, b, 0.5f);
    float len = std::sqrt(mid.x * mid.x + mid.y * mid.y + mid.z * mid.z + mid.w * mid.w);
    EXPECT_NEAR(len, 1.0f, 1e-5f);
}

// ============================================================================
// Matrix4::look_at
// ============================================================================

TEST(Matrix4, LookAtForwardIsNegativeZ)
{
    // Camera at origin looking toward +Z (standard OpenGL/row-major convention:
    // forward stored as -Z in the view row).
    auto m = Matrix4::look_at({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    // The third row (row 2) of a look_at encodes -forward in our convention.
    Vector3 neg_forward{m(2, 0), m(2, 1), m(2, 2)};
    EXPECT_NEAR(neg_forward.x, 0.0f, 1e-5f);
    EXPECT_NEAR(neg_forward.y, 0.0f, 1e-5f);
    EXPECT_NEAR(neg_forward.z, -1.0f, 1e-5f);
}

TEST(Matrix4, LookAtTransformsEyeToOrigin)
{
    Vector3 eye{3, 4, 5};
    auto m = Matrix4::look_at(eye, {0, 0, 0}, {0, 1, 0});
    // The view matrix should map the eye position to (0,0,0) in view space.
    auto p = m.transform_point(eye);
    EXPECT_NEAR(p.x, 0.0f, 1e-4f);
    EXPECT_NEAR(p.y, 0.0f, 1e-4f);
    EXPECT_NEAR(p.z, 0.0f, 1e-4f);
}

TEST(Matrix4, LookAtRightVectorIsOrthogonalToUp)
{
    auto m = Matrix4::look_at({1, 2, 3}, {4, 5, 6}, {0, 1, 0});
    // Row 0 is "right", row 1 is "up" in our look_at
    Vector3 right{m(0, 0), m(0, 1), m(0, 2)};
    Vector3 up{m(1, 0), m(1, 1), m(1, 2)};
    EXPECT_NEAR(right.dot(up), 0.0f, 1e-5f);
}

// ============================================================================
// Matrix4::transposed
// ============================================================================

TEST(Matrix4, TransposedSwapsRowAndColumn)
{
    auto m = Matrix4::translation({1, 2, 3});
    auto t = m.transposed();
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            EXPECT_NEAR(t(row, col), m(col, row), 1e-6f) << "at [" << row << "," << col << "]";
        }
    }
}

TEST(Matrix4, TransposedTwiceIsOriginal)
{
    auto m = Matrix4::rotation_z(0.7f);
    auto tt = m.transposed().transposed();
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            EXPECT_NEAR(tt(row, col), m(row, col), 1e-6f);
        }
    }
}

TEST(Matrix4, TransposedIdentityIsIdentity)
{
    auto id = Matrix4::identity();
    auto t = id.transposed();
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            EXPECT_NEAR(t(i, j), id(i, j), 1e-6f);
        }
    }
}

// ============================================================================
// Quaternion::operator*  (composition)
// ============================================================================

TEST(Quaternion, MultiplyRotationsCompose)
{
    // 90° around Z twice = 180° around Z
    auto q90 = Quaternion::from_axis_angle(Vector3::unit_z(), kHalfPi);
    auto q180 = q90 * q90;

    // Rotating unit_x by 180° around Z should yield -unit_x
    auto v = q180.rotate(Vector3::unit_x());
    EXPECT_NEAR(v.x, -1.0f, 1e-4f);
    EXPECT_NEAR(v.y, 0.0f, 1e-4f);
    EXPECT_NEAR(v.z, 0.0f, 1e-4f);
}

TEST(Quaternion, MultiplyByIdentityIsNoop)
{
    auto q = Quaternion::from_axis_angle({1, 1, 0}, deg_to_rad(45.0f));
    auto result = q * Quaternion::identity();
    EXPECT_NEAR(result.x, q.x, 1e-5f);
    EXPECT_NEAR(result.y, q.y, 1e-5f);
    EXPECT_NEAR(result.z, q.z, 1e-5f);
    EXPECT_NEAR(result.w, q.w, 1e-5f);
}

TEST(Quaternion, MultiplyResultIsNormalized)
{
    auto a = Quaternion::from_axis_angle(Vector3::unit_x(), deg_to_rad(30.0f));
    auto b = Quaternion::from_axis_angle(Vector3::unit_y(), deg_to_rad(60.0f));
    auto c = a * b;
    EXPECT_NEAR(c.length(), 1.0f, 1e-5f);
}

// ============================================================================
// Quaternion::inversed
// ============================================================================

TEST(Quaternion, InversedTimesOriginalIsIdentity)
{
    auto q = Quaternion::from_axis_angle({1, 1, 1}, deg_to_rad(72.0f));
    auto q_inv = q.inversed();
    auto product = q * q_inv;

    EXPECT_NEAR(product.x, 0.0f, 1e-5f);
    EXPECT_NEAR(product.y, 0.0f, 1e-5f);
    EXPECT_NEAR(product.z, 0.0f, 1e-5f);
    EXPECT_NEAR(product.w, 1.0f, 1e-5f);
}

TEST(Quaternion, InversedOfIdentityIsIdentity)
{
    auto id = Quaternion::identity();
    auto inv = id.inversed();
    EXPECT_NEAR(inv.x, 0.0f, 1e-5f);
    EXPECT_NEAR(inv.y, 0.0f, 1e-5f);
    EXPECT_NEAR(inv.z, 0.0f, 1e-5f);
    EXPECT_NEAR(inv.w, 1.0f, 1e-5f);
}

TEST(Quaternion, InversedRevertsRotation)
{
    auto q = Quaternion::from_axis_angle(Vector3::unit_y(), deg_to_rad(90.0f));
    auto q_inv = q.inversed();
    // Rotate then un-rotate should return the original vector
    Vector3 v{1, 0, 0};
    auto rotated = q.rotate(v);
    auto restored = q_inv.rotate(rotated);
    EXPECT_NEAR(restored.x, v.x, 1e-4f);
    EXPECT_NEAR(restored.y, v.y, 1e-4f);
    EXPECT_NEAR(restored.z, v.z, 1e-4f);
}

// ============================================================================
// Quaternion::from_euler
// ============================================================================

TEST(Quaternion, FromEulerPureYawMatchesAxisAngle)
{
    // Pure yaw (rotation around Y) should match from_axis_angle(unit_y, angle)
    float angle = deg_to_rad(45.0f);
    auto q_euler = Quaternion::from_euler(0.0f, angle, 0.0f);
    auto q_axis = Quaternion::from_axis_angle(Vector3::unit_y(), angle);

    // Both should rotate unit_x the same way
    auto v_euler = q_euler.rotate(Vector3::unit_x());
    auto v_axis = q_axis.rotate(Vector3::unit_x());
    EXPECT_NEAR(v_euler.x, v_axis.x, 1e-4f);
    EXPECT_NEAR(v_euler.y, v_axis.y, 1e-4f);
    EXPECT_NEAR(v_euler.z, v_axis.z, 1e-4f);
}

TEST(Quaternion, FromEulerZeroAnglesIsIdentity)
{
    auto q = Quaternion::from_euler(0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(q.x, 0.0f, 1e-5f);
    EXPECT_NEAR(q.y, 0.0f, 1e-5f);
    EXPECT_NEAR(q.z, 0.0f, 1e-5f);
    EXPECT_NEAR(q.w, 1.0f, 1e-5f);
}

TEST(Quaternion, FromEulerResultIsNormalized)
{
    auto q = Quaternion::from_euler(0.3f, 0.7f, 1.2f);
    EXPECT_NEAR(q.length(), 1.0f, 1e-5f);
}
