#include "math/matrix4.h"

#include "math/quaternion.h"

namespace atlas::math {

auto Matrix4::RotationX(float radians) -> Matrix4 {
  Matrix4 result = Identity();
  float c = std::cos(radians);
  float s = std::sin(radians);
  result(1, 1) = c;
  result(1, 2) = -s;
  result(2, 1) = s;
  result(2, 2) = c;
  return result;
}

auto Matrix4::RotationY(float radians) -> Matrix4 {
  Matrix4 result = Identity();
  float c = std::cos(radians);
  float s = std::sin(radians);
  result(0, 0) = c;
  result(0, 2) = s;
  result(2, 0) = -s;
  result(2, 2) = c;
  return result;
}

auto Matrix4::RotationZ(float radians) -> Matrix4 {
  Matrix4 result = Identity();
  float c = std::cos(radians);
  float s = std::sin(radians);
  result(0, 0) = c;
  result(0, 1) = -s;
  result(1, 0) = s;
  result(1, 1) = c;
  return result;
}

auto Matrix4::FromQuaternion(const Quaternion& q) -> Matrix4 {
  return q.ToMatrix();
}

auto Matrix4::LookAt(const Vector3& eye, const Vector3& target, const Vector3& up) -> Matrix4 {
  Vector3 forward = (target - eye).Normalized();
  Vector3 right = forward.Cross(up).Normalized();
  Vector3 cam_up = right.Cross(forward);

  Matrix4 result = Identity();

  result(0, 0) = right.x;
  result(0, 1) = right.y;
  result(0, 2) = right.z;
  result(0, 3) = -right.Dot(eye);

  result(1, 0) = cam_up.x;
  result(1, 1) = cam_up.y;
  result(1, 2) = cam_up.z;
  result(1, 3) = -cam_up.Dot(eye);

  result(2, 0) = -forward.x;
  result(2, 1) = -forward.y;
  result(2, 2) = -forward.z;
  result(2, 3) = forward.Dot(eye);

  return result;
}

auto Matrix4::operator*(const Matrix4& other) const -> Matrix4 {
  Matrix4 result;
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += (*this)(row, k) * other(k, col);
      }
      result(row, col) = sum;
    }
  }
  return result;
}

auto Matrix4::TransformPoint(const Vector3& p) const -> Vector3 {
  float rx = (*this)(0, 0) * p.x + (*this)(0, 1) * p.y + (*this)(0, 2) * p.z + (*this)(0, 3);
  float ry = (*this)(1, 0) * p.x + (*this)(1, 1) * p.y + (*this)(1, 2) * p.z + (*this)(1, 3);
  float rz = (*this)(2, 0) * p.x + (*this)(2, 1) * p.y + (*this)(2, 2) * p.z + (*this)(2, 3);
  return {rx, ry, rz};
}

auto Matrix4::TransformVector(const Vector3& v) const -> Vector3 {
  float rx = (*this)(0, 0) * v.x + (*this)(0, 1) * v.y + (*this)(0, 2) * v.z;
  float ry = (*this)(1, 0) * v.x + (*this)(1, 1) * v.y + (*this)(1, 2) * v.z;
  float rz = (*this)(2, 0) * v.x + (*this)(2, 1) * v.y + (*this)(2, 2) * v.z;
  return {rx, ry, rz};
}

auto Matrix4::Transposed() const -> Matrix4 {
  Matrix4 result;
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      result(row, col) = (*this)(col, row);
    }
  }
  return result;
}

auto Matrix4::Determinant() const -> float {
  float a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3];
  float a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
  float a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11];
  float a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];

  float c00 =
      a11 * (a22 * a33 - a23 * a32) - a12 * (a21 * a33 - a23 * a31) + a13 * (a21 * a32 - a22 * a31);

  float c01 =
      a10 * (a22 * a33 - a23 * a32) - a12 * (a20 * a33 - a23 * a30) + a13 * (a20 * a32 - a22 * a30);

  float c02 =
      a10 * (a21 * a33 - a23 * a31) - a11 * (a20 * a33 - a23 * a30) + a13 * (a20 * a31 - a21 * a30);

  float c03 =
      a10 * (a21 * a32 - a22 * a31) - a11 * (a20 * a32 - a22 * a30) + a12 * (a20 * a31 - a21 * a30);

  return a00 * c00 - a01 * c01 + a02 * c02 - a03 * c03;
}

auto Matrix4::Inversed() const -> Matrix4 {
  float a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3];
  float a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
  float a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11];
  float a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];

  float s0 = a00 * a11 - a10 * a01;
  float s1 = a00 * a12 - a10 * a02;
  float s2 = a00 * a13 - a10 * a03;
  float s3 = a01 * a12 - a11 * a02;
  float s4 = a01 * a13 - a11 * a03;
  float s5 = a02 * a13 - a12 * a03;

  float c5 = a22 * a33 - a32 * a23;
  float c4 = a21 * a33 - a31 * a23;
  float c3 = a21 * a32 - a31 * a22;
  float c2 = a20 * a33 - a30 * a23;
  float c1 = a20 * a32 - a30 * a22;
  float c0 = a20 * a31 - a30 * a21;

  float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;

  if (det > -kEpsilon && det < kEpsilon) {
    return Identity();
  }

  float inv_det = 1.0f / det;

  Matrix4 result;

  result.m[0] = (a11 * c5 - a12 * c4 + a13 * c3) * inv_det;
  result.m[1] = (-a01 * c5 + a02 * c4 - a03 * c3) * inv_det;
  result.m[2] = (a31 * s5 - a32 * s4 + a33 * s3) * inv_det;
  result.m[3] = (-a21 * s5 + a22 * s4 - a23 * s3) * inv_det;

  result.m[4] = (-a10 * c5 + a12 * c2 - a13 * c1) * inv_det;
  result.m[5] = (a00 * c5 - a02 * c2 + a03 * c1) * inv_det;
  result.m[6] = (-a30 * s5 + a32 * s2 - a33 * s1) * inv_det;
  result.m[7] = (a20 * s5 - a22 * s2 + a23 * s1) * inv_det;

  result.m[8] = (a10 * c4 - a11 * c2 + a13 * c0) * inv_det;
  result.m[9] = (-a00 * c4 + a01 * c2 - a03 * c0) * inv_det;
  result.m[10] = (a30 * s4 - a31 * s2 + a33 * s0) * inv_det;
  result.m[11] = (-a20 * s4 + a21 * s2 - a23 * s0) * inv_det;

  result.m[12] = (-a10 * c3 + a11 * c1 - a12 * c0) * inv_det;
  result.m[13] = (a00 * c3 - a01 * c1 + a02 * c0) * inv_det;
  result.m[14] = (-a30 * s3 + a31 * s1 - a32 * s0) * inv_det;
  result.m[15] = (a20 * s3 - a21 * s1 + a22 * s0) * inv_det;

  return result;
}

auto Matrix4::GetScale() const -> Vector3 {
  // Row-major: row i is m[i*4 .. i*4+3]. Scale is the length of each row's
  // xyz part (i.e., the basis vectors stored in rows 0-2).
  float sx = Vector3{m[0], m[1], m[2]}.Length();
  float sy = Vector3{m[4], m[5], m[6]}.Length();
  float sz = Vector3{m[8], m[9], m[10]}.Length();
  return {sx, sy, sz};
}

}  // namespace atlas::math
