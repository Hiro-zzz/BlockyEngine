#pragma once
// Minimal, engine-owned linear algebra. Right-handed, Y-up, column-vector
// convention: a transform is applied as M * v.
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace blocky {

inline constexpr float kPi    = 3.14159265358979323846f;
inline constexpr float kInvPi = 0.31830988618379067154f;
inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kEps   = 1e-6f;

inline constexpr float radians(float deg) { return deg * (kPi / 180.0f); }
inline constexpr float degrees(float rad) { return rad * (180.0f / kPi); }
inline constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline constexpr float saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// ---------------------------------------------------------------------- Vec3
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr float  operator[](int i) const { return (&x)[i]; }
    constexpr float& operator[](int i)       { return (&x)[i]; }

    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
constexpr Vec3 operator/(Vec3 a, Vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr Vec3 operator*(float s, Vec3 a) { return a * s; }
constexpr Vec3 operator/(Vec3 a, float s) { return a * (1.0f / s); }

constexpr Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
constexpr Vec3& operator-=(Vec3& a, Vec3 b) { a = a - b; return a; }
constexpr Vec3& operator*=(Vec3& a, Vec3 b) { a = a * b; return a; }
constexpr Vec3& operator*=(Vec3& a, float s) { a = a * s; return a; }
constexpr Vec3& operator/=(Vec3& a, float s) { a = a / s; return a; }

constexpr bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3  cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline    float length(Vec3 v)   { return std::sqrt(dot(v, v)); }
constexpr float lengthSq(Vec3 v) { return dot(v, v); }
inline    Vec3  normalize(Vec3 v) { float l = length(v); return l > kEps ? v / l : Vec3{}; }

constexpr Vec3 minv(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
constexpr Vec3 maxv(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline    Vec3 absv(Vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
constexpr float maxComponent(Vec3 v) { return std::max(v.x, std::max(v.y, v.z)); }
constexpr float minComponent(Vec3 v) { return std::min(v.x, std::min(v.y, v.z)); }

// Reflect v about unit normal n.
constexpr Vec3 reflect(Vec3 v, Vec3 n) { return v - n * (2.0f * dot(v, n)); }

// Branchless orthonormal basis around unit vector n (Duff et al. 2017).
inline void orthonormalBasis(Vec3 n, Vec3& t, Vec3& b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float d = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * d, -sign * n.x};
    b = {d, sign + n.y * n.y * a, -n.y};
}

// ---------------------------------------------------------------------- Vec2
struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
};
constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }

// --------------------------------------------------------------------- IVec3
// Integer lattice coordinate: a block position, a chunk index, a grid size.
struct IVec3 {
    int32_t x = 0, y = 0, z = 0;
    constexpr IVec3() = default;
    constexpr explicit IVec3(int32_t s) : x(s), y(s), z(s) {}
    constexpr IVec3(int32_t x_, int32_t y_, int32_t z_) : x(x_), y(y_), z(z_) {}

    constexpr int32_t  operator[](int i) const { return (&x)[i]; }
    constexpr int32_t& operator[](int i)       { return (&x)[i]; }
};
constexpr IVec3 operator+(IVec3 a, IVec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr IVec3 operator-(IVec3 a, IVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr IVec3 operator*(IVec3 a, int32_t s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr bool  operator==(IVec3 a, IVec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
constexpr bool  operator!=(IVec3 a, IVec3 b) { return !(a == b); }
constexpr IVec3 minv(IVec3 a, IVec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
constexpr IVec3 maxv(IVec3 a, IVec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
constexpr Vec3  toVec3(IVec3 v) { return {float(v.x), float(v.y), float(v.z)}; }
inline    IVec3 floorToInt(Vec3 v) {
    return {int32_t(std::floor(v.x)), int32_t(std::floor(v.y)), int32_t(std::floor(v.z))};
}

// ---------------------------------------------------------------------- Mat4
// Column-major storage, matching OpenGL: m[column][row].
struct Mat4 {
    float m[4][4]{};

    static constexpr Mat4 identity() {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
    const float* data() const { return &m[0][0]; }
};

constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k][i] * b.m[c][k];
            r.m[c][i] = s;
        }
    return r;
}

// Transform a point (w = 1) or a direction (w = 0).
constexpr Vec3 transformPoint(const Mat4& t, Vec3 p) {
    return {t.m[0][0]*p.x + t.m[1][0]*p.y + t.m[2][0]*p.z + t.m[3][0],
            t.m[0][1]*p.x + t.m[1][1]*p.y + t.m[2][1]*p.z + t.m[3][1],
            t.m[0][2]*p.x + t.m[1][2]*p.y + t.m[2][2]*p.z + t.m[3][2]};
}
constexpr Vec3 transformDir(const Mat4& t, Vec3 v) {
    return {t.m[0][0]*v.x + t.m[1][0]*v.y + t.m[2][0]*v.z,
            t.m[0][1]*v.x + t.m[1][1]*v.y + t.m[2][1]*v.z,
            t.m[0][2]*v.x + t.m[1][2]*v.y + t.m[2][2]*v.z};
}

// Inverse of an affine transform (any 3x3 linear part plus a translation).
// Entity boxes are built as a chain of translations, rotations and a uniform
// scale, and a ray has to be pushed into box-local space to be intersected.
inline Mat4 inverseAffine(const Mat4& m) {
    // Cofactor inverse of the upper 3x3.
    float a00 = m.m[0][0], a01 = m.m[1][0], a02 = m.m[2][0];
    float a10 = m.m[0][1], a11 = m.m[1][1], a12 = m.m[2][1];
    float a20 = m.m[0][2], a21 = m.m[1][2], a22 = m.m[2][2];

    float c00 =  (a11 * a22 - a12 * a21);
    float c01 = -(a10 * a22 - a12 * a20);
    float c02 =  (a10 * a21 - a11 * a20);

    float determinant = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(determinant) < 1e-20f) return Mat4::identity();
    float inv = 1.0f / determinant;

    Mat4 r = Mat4::identity();
    r.m[0][0] = c00 * inv;
    r.m[0][1] = c01 * inv;
    r.m[0][2] = c02 * inv;
    r.m[1][0] = -(a01 * a22 - a02 * a21) * inv;
    r.m[1][1] =  (a00 * a22 - a02 * a20) * inv;
    r.m[1][2] = -(a00 * a21 - a01 * a20) * inv;
    r.m[2][0] =  (a01 * a12 - a02 * a11) * inv;
    r.m[2][1] = -(a00 * a12 - a02 * a10) * inv;
    r.m[2][2] =  (a00 * a11 - a01 * a10) * inv;

    Vec3 translation{m.m[3][0], m.m[3][1], m.m[3][2]};
    Vec3 inverseTranslation = transformDir(r, translation);
    r.m[3][0] = -inverseTranslation.x;
    r.m[3][1] = -inverseTranslation.y;
    r.m[3][2] = -inverseTranslation.z;
    return r;
}

constexpr Mat4 translate(Vec3 t) {
    Mat4 r = Mat4::identity();
    r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
    return r;
}
constexpr Mat4 scale(Vec3 s) {
    Mat4 r{};
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z; r.m[3][3] = 1.0f;
    return r;
}
inline Mat4 rotateAxis(Vec3 axis, float angleRad) {
    Vec3 a = normalize(axis);
    float s = std::sin(angleRad), c = std::cos(angleRad), t = 1.0f - c;
    Mat4 r = Mat4::identity();
    r.m[0][0] = t*a.x*a.x + c;     r.m[1][0] = t*a.x*a.y - s*a.z; r.m[2][0] = t*a.x*a.z + s*a.y;
    r.m[0][1] = t*a.x*a.y + s*a.z; r.m[1][1] = t*a.y*a.y + c;     r.m[2][1] = t*a.y*a.z - s*a.x;
    r.m[0][2] = t*a.x*a.z - s*a.y; r.m[1][2] = t*a.y*a.z + s*a.x; r.m[2][2] = t*a.z*a.z + c;
    return r;
}

// Right-handed look-at: builds a world -> view matrix.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(target - eye);  // forward
    Vec3 s = normalize(cross(f, up));  // right
    Vec3 u = cross(s, f);              // true up
    Mat4 r = Mat4::identity();
    r.m[0][0] =  s.x; r.m[1][0] =  s.y; r.m[2][0] =  s.z;
    r.m[0][1] =  u.x; r.m[1][1] =  u.y; r.m[2][1] =  u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] =  dot(f, eye);
    return r;
}

// Right-handed perspective mapping to the OpenGL clip range z in [-1, 1].
inline Mat4 perspective(float fovYRad, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fovYRad * 0.5f);
    Mat4 r{};
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (zFar + zNear) / (zNear - zFar);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (2.0f * zFar * zNear) / (zNear - zFar);
    return r;
}

// Orthographic projection, for isometric-style renders.
inline Mat4 orthographic(float halfWidth, float halfHeight, float zNear, float zFar) {
    Mat4 r = Mat4::identity();
    r.m[0][0] =  1.0f / halfWidth;
    r.m[1][1] =  1.0f / halfHeight;
    r.m[2][2] = -2.0f / (zFar - zNear);
    r.m[3][2] = -(zFar + zNear) / (zFar - zNear);
    return r;
}

// ---------------------------------------------------------------------- Mat3
// The linear part on its own, column-major like Mat4. It exists for inertia
// tensors: a rigid body's is a genuine 3x3 that gets rotated into world space
// every step as R * I * R^T, and carrying it inside a Mat4 would mean the
// translation column silently taking part in that product.
struct Mat3 {
    float m[3][3]{};

    static Mat3 identity() {
        Mat3 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
        return r;
    }

    static Mat3 diagonal(Vec3 d) {
        Mat3 r;
        r.m[0][0] = d.x; r.m[1][1] = d.y; r.m[2][2] = d.z;
        return r;
    }
};

inline Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            r.m[c][i] = a.m[0][i] * b.m[c][0] + a.m[1][i] * b.m[c][1] + a.m[2][i] * b.m[c][2];
    return r;
}

inline Vec3 operator*(const Mat3& a, Vec3 v) {
    return {a.m[0][0] * v.x + a.m[1][0] * v.y + a.m[2][0] * v.z,
            a.m[0][1] * v.x + a.m[1][1] * v.y + a.m[2][1] * v.z,
            a.m[0][2] * v.x + a.m[1][2] * v.y + a.m[2][2] * v.z};
}

inline Mat3 transpose(const Mat3& a) {
    Mat3 r;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i) r.m[c][i] = a.m[i][c];
    return r;
}

// General inverse, because a body assembled from several boxes has a fully
// populated inertia tensor -- the parallel-axis terms are off-diagonal as soon
// as a box sits away from the centre of mass on two axes at once.
inline Mat3 inverse(const Mat3& a) {
    float c00 =  (a.m[1][1] * a.m[2][2] - a.m[2][1] * a.m[1][2]);
    float c01 = -(a.m[1][0] * a.m[2][2] - a.m[2][0] * a.m[1][2]);
    float c02 =  (a.m[1][0] * a.m[2][1] - a.m[2][0] * a.m[1][1]);

    float det = a.m[0][0] * c00 + a.m[0][1] * c01 + a.m[0][2] * c02;
    if (std::fabs(det) < 1e-20f) return Mat3{};  // singular: caller gets zeros
    float invDet = 1.0f / det;

    Mat3 r;
    r.m[0][0] = c00 * invDet;
    r.m[0][1] = -(a.m[0][1] * a.m[2][2] - a.m[2][1] * a.m[0][2]) * invDet;
    r.m[0][2] =  (a.m[0][1] * a.m[1][2] - a.m[1][1] * a.m[0][2]) * invDet;
    r.m[1][0] = c01 * invDet;
    r.m[1][1] =  (a.m[0][0] * a.m[2][2] - a.m[2][0] * a.m[0][2]) * invDet;
    r.m[1][2] = -(a.m[0][0] * a.m[1][2] - a.m[1][0] * a.m[0][2]) * invDet;
    r.m[2][0] = c02 * invDet;
    r.m[2][1] = -(a.m[0][0] * a.m[2][1] - a.m[2][0] * a.m[0][1]) * invDet;
    r.m[2][2] =  (a.m[0][0] * a.m[1][1] - a.m[1][0] * a.m[0][1]) * invDet;
    return r;
}

// ---------------------------------------------------------------------- Quat
// Orientation of a freely tumbling body. Euler angles are exact everywhere
// else in this engine because every joint it has is a single-axis hinge --
// `Prop` stores three angles for the same reason. A body under torque turns
// about an axis that moves, which is the case that argument was always
// written to exclude.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    static Quat identity() { return {}; }

    static Quat axisAngle(Vec3 axis, float angleRad) {
        Vec3 a = normalize(axis);
        float h = angleRad * 0.5f;
        float s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }
};

inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat operator*(Quat q, float s) { return {q.x * s, q.y * s, q.z * s, q.w * s}; }
inline Quat operator+(Quat a, Quat b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }

inline Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat normalize(Quat q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > kEps ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat::identity();
}

inline Vec3 rotate(Quat q, Vec3 v) {
    // v + 2w(u x v) + 2(u x (u x v)), with u the vector part -- the usual
    // expansion, which avoids building a matrix for a single vector.
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

inline Mat3 toMat3(Quat q) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    Mat3 r;
    r.m[0][0] = 1.0f - 2.0f * (yy + zz);
    r.m[0][1] = 2.0f * (xy + wz);
    r.m[0][2] = 2.0f * (xz - wy);
    r.m[1][0] = 2.0f * (xy - wz);
    r.m[1][1] = 1.0f - 2.0f * (xx + zz);
    r.m[1][2] = 2.0f * (yz + wx);
    r.m[2][0] = 2.0f * (xz + wy);
    r.m[2][1] = 2.0f * (yz - wx);
    r.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return r;
}

// Rotation, then uniform scale, then translation -- the rigid-plus-uniform
// form `PropSet::addTransformed` requires. Assembling it here rather than in
// the caller keeps the one place that knows the order.
inline Mat4 composeTransform(Vec3 translation, Quat rotation, float uniformScale) {
    Mat3 basis = toMat3(rotation);
    Mat4 r = Mat4::identity();
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i) r.m[c][i] = basis.m[c][i] * uniformScale;
    r.m[3][0] = translation.x;
    r.m[3][1] = translation.y;
    r.m[3][2] = translation.z;
    return r;
}

} // namespace blocky
