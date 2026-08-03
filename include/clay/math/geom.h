#pragma once

// Host-side geometry: AABB, ray, plane, frustum. Uses the kernel's cfloat
// types so values flow between host structures and kernels without
// conversion.

#include <cfloat>

#include "clay/kernel/shim.h"

namespace clay {
namespace math {

using kernel::cf2;
using kernel::cf3;
using kernel::cfloat2;
using kernel::cfloat3;
using kernel::cfloat4;
using kernel::cfloat4x4;

struct Aabb {
    cfloat3 min = cf3(FLT_MAX, FLT_MAX, FLT_MAX);
    cfloat3 max = cf3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    static Aabb infinite() {
        return Aabb{cf3(-FLT_MAX, -FLT_MAX, -FLT_MAX), cf3(FLT_MAX, FLT_MAX, FLT_MAX)};
    }

    bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }
    bool is_infinite() const { return !empty() && (min.x == -FLT_MAX || max.x == FLT_MAX); }

    cfloat3 center() const { return (min + max) * 0.5f; }
    cfloat3 extent() const { return max - min; }

    void expand(cfloat3 p) {
        min = kernel::cmin(min, p);
        max = kernel::cmax(max, p);
    }
    void expand(const Aabb& b) {
        if (b.empty()) return;
        min = kernel::cmin(min, b.min);
        max = kernel::cmax(max, b.max);
    }

    Aabb dilated(float r) const {
        if (empty()) return *this;
        return Aabb{min - cf3(r, r, r), max + cf3(r, r, r)};
    }

    bool intersects(const Aabb& b) const {
        if (empty() || b.empty()) return false;
        return min.x <= b.max.x && max.x >= b.min.x && min.y <= b.max.y && max.y >= b.min.y &&
               min.z <= b.max.z && max.z >= b.min.z;
    }

    bool contains(cfloat3 p) const {
        return !empty() && p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y &&
               p.z >= min.z && p.z <= max.z;
    }

    // Euclidean distance from a point to the box (0 inside).
    float distance(cfloat3 p) const {
        cfloat3 d = kernel::cmax(kernel::cmax(min - p, p - max), 0.0f);
        return kernel::clength(d);
    }

    // AABB of this box under an affine transform (8 transformed corners).
    Aabb transformed(const cfloat4x4& m) const {
        if (empty() || is_infinite()) return *this;
        Aabb out;
        for (int i = 0; i < 8; ++i) {
            cfloat3 c = cf3((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y,
                            (i & 4) ? max.z : min.z);
            out.expand(kernel::cmul_point(m, c));
        }
        return out;
    }
};

struct Ray {
    cfloat3 origin;
    cfloat3 dir;  // normalized
    cfloat3 at(float t) const { return origin + dir * t; }
};

// Slab test; returns entry/exit in *t0/*t1 when hit.
inline bool ray_aabb(const Ray& r, const Aabb& b, float* t0, float* t1) {
    if (b.empty()) return false;
    float lo = -FLT_MAX, hi = FLT_MAX;
    const float ro[3] = {r.origin.x, r.origin.y, r.origin.z};
    const float rd[3] = {r.dir.x, r.dir.y, r.dir.z};
    const float bmin[3] = {b.min.x, b.min.y, b.min.z};
    const float bmax[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        if (kernel::cabs(rd[i]) < 1e-12f) {
            if (ro[i] < bmin[i] || ro[i] > bmax[i]) return false;
            continue;
        }
        float inv = 1.0f / rd[i];
        float a = (bmin[i] - ro[i]) * inv;
        float c = (bmax[i] - ro[i]) * inv;
        if (a > c) {
            float tmp = a;
            a = c;
            c = tmp;
        }
        lo = kernel::cmax(lo, a);
        hi = kernel::cmin(hi, c);
        if (lo > hi) return false;
    }
    *t0 = lo;
    *t1 = hi;
    return true;
}

// Plane n·p + d = 0, n normalized, positive half-space is "inside".
struct Plane {
    cfloat3 n;
    float d;
    float signed_distance(cfloat3 p) const { return kernel::cdot(n, p) + d; }
};

// Six planes, normals pointing inward.
struct Frustum {
    Plane planes[6];

    bool intersects(const Aabb& b) const {
        if (b.empty()) return false;
        for (const Plane& pl : planes) {
            // p-vertex test: farthest corner along the normal
            cfloat3 p = cf3(pl.n.x >= 0 ? b.max.x : b.min.x, pl.n.y >= 0 ? b.max.y : b.min.y,
                            pl.n.z >= 0 ? b.max.z : b.min.z);
            if (pl.signed_distance(p) < 0) return false;
        }
        return true;
    }
};

}  // namespace math
}  // namespace clay
