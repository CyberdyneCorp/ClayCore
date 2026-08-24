#pragma once

// Quaternion and rigid+uniform-scale transform. This is the transform every
// scene item and layer carries; tape compilation pre-inverts it into a
// cfloat4x4 for the kernels (scene-model spec).

#include <cmath>

#include "clay/kernel/shim.h"

namespace clay {
namespace math {

using kernel::cf3;
using kernel::cf4;
using kernel::cfloat3;
using kernel::cfloat4;
using kernel::cfloat4x4;

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;

    static Quat identity() { return Quat{}; }

    static Quat from_axis_angle(cfloat3 axis, float radians) {
        cfloat3 a = kernel::cnormalize(axis);
        float s = kernel::csin(radians * 0.5f);
        return Quat{a.x * s, a.y * s, a.z * s, kernel::ccos(radians * 0.5f)};
    }

    // The rotation taking the world axes onto the given orthonormal basis, so
    // that x -> right, y -> up, z -> forward. Shepperd's method: pick the
    // largest diagonal term to divide by, which keeps it stable for every
    // basis rather than only those away from a 180-degree turn.
    static Quat from_basis(cfloat3 right, cfloat3 up, cfloat3 forward) {
        float m00 = right.x, m01 = up.x, m02 = forward.x;
        float m10 = right.y, m11 = up.y, m12 = forward.y;
        float m20 = right.z, m21 = up.z, m22 = forward.z;
        float trace = m00 + m11 + m22;
        Quat q;
        if (trace > 0.0f) {
            float s = kernel::csqrt(trace + 1.0f) * 2.0f;
            q = Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
        } else if (m00 > m11 && m00 > m22) {
            float s = kernel::csqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = Quat{0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
        } else if (m11 > m22) {
            float s = kernel::csqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = Quat{(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
        } else {
            float s = kernel::csqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
        }
        return q.normalized();
    }

    Quat conjugate() const { return Quat{-x, -y, -z, w}; }

    Quat normalized() const {
        float n = kernel::csqrt(x * x + y * y + z * z + w * w);
        return Quat{x / n, y / n, z / n, w / n};
    }

    friend Quat operator*(const Quat& a, const Quat& b) {
        return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    cfloat3 rotate(cfloat3 v) const {
        cfloat3 u = cf3(x, y, z);
        cfloat3 t = kernel::ccross(u, v) * 2.0f;
        return v + t * w + kernel::ccross(u, t);
    }
};

struct Transform {
    cfloat3 position = cf3(0, 0, 0);
    Quat rotation = Quat::identity();
    float scale = 1.0f;  // uniform (exact); non-uniform scale is an operator

    static Transform identity() { return Transform{}; }

    cfloat3 apply(cfloat3 p) const { return rotation.rotate(p * scale) + position; }
    cfloat3 apply_inverse(cfloat3 p) const {
        return rotation.conjugate().rotate(p - position) / scale;
    }

    Transform inverse() const {
        Quat ri = rotation.conjugate();
        return Transform{ri.rotate(-position) / scale, ri, 1.0f / scale};
    }

    // Composition: (a * b).apply(p) == a.apply(b.apply(p))
    friend Transform operator*(const Transform& a, const Transform& b) {
        return Transform{a.apply(b.position), (a.rotation * b.rotation).normalized(),
                         a.scale * b.scale};
    }

    // Column-major affine matrix (rotation*scale | translation).
    cfloat4x4 matrix() const {
        cfloat3 cx = rotation.rotate(cf3(scale, 0, 0));
        cfloat3 cy = rotation.rotate(cf3(0, scale, 0));
        cfloat3 cz = rotation.rotate(cf3(0, 0, scale));
        return cfloat4x4{cf4(cx.x, cx.y, cx.z, 0), cf4(cy.x, cy.y, cy.z, 0),
                         cf4(cz.x, cz.y, cz.z, 0), cf4(position.x, position.y, position.z, 1)};
    }

    cfloat4x4 inverse_matrix() const { return inverse().matrix(); }
};

// Affine matrix product (needed for mirror reflections, which have det -1
// and cannot be expressed as a Transform).
inline cfloat4x4 mul(const cfloat4x4& a, const cfloat4x4& b) {
    auto mulc = [&](cfloat4 c) {
        return a.c0 * c.x + a.c1 * c.y + a.c2 * c.z + a.c3 * c.w;
    };
    return cfloat4x4{mulc(b.c0), mulc(b.c1), mulc(b.c2), mulc(b.c3)};
}

// Rotation about the local axis 0/1/2, by `radians`.
//
// Built as a matrix rather than a Transform even though a rotation, unlike a
// reflection, has determinant +1 and would express as one. The layer's radial
// symmetry composes it exactly where the mirror composes a reflection —
// `item^-1 * R * layer^-1` — so keeping the two the same shape is what lets a
// reader see one pattern instead of noticing that one of them is special.
inline cfloat4x4 rotation_matrix(int axis, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    cfloat4x4 m{cf4(1, 0, 0, 0), cf4(0, 1, 0, 0), cf4(0, 0, 1, 0), cf4(0, 0, 0, 1)};
    if (axis == 0) {
        m.c1.y = c;  m.c1.z = s;
        m.c2.y = -s; m.c2.z = c;
    } else if (axis == 1) {
        m.c0.x = c;  m.c0.z = -s;
        m.c2.x = s;  m.c2.z = c;
    } else {
        m.c0.x = c;  m.c0.y = s;
        m.c1.x = -s; m.c1.y = c;
    }
    return m;
}

// Reflection across the local plane whose normal is axis 0/1/2.
inline cfloat4x4 reflection_matrix(int axis) {
    cfloat4x4 m{cf4(1, 0, 0, 0), cf4(0, 1, 0, 0), cf4(0, 0, 1, 0), cf4(0, 0, 0, 1)};
    if (axis == 0) m.c0.x = -1;
    if (axis == 1) m.c1.y = -1;
    if (axis == 2) m.c2.z = -1;
    return m;
}

}  // namespace math
}  // namespace clay
