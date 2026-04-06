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
