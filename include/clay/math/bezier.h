#pragma once

// Host-side cubic -> quadratic Bézier conversion. Quintic root-finding is
// ruled out in-kernel (docs/01 §1.3): the scene compiler flattens cubics
// into quadratic chains with this utility and kernels evaluate
// sd_bezier2 per quadratic segment.

#include <cstddef>
#include <vector>

#include "clay/kernel/prim2d.h"

namespace clay {
namespace math {

struct QuadBezier {
    kernel::cfloat2 a, b, c;
};

namespace detail {

struct Cubic {
    kernel::cfloat2 p0, p1, p2, p3;
};

inline kernel::cfloat2 lerp2(kernel::cfloat2 a, kernel::cfloat2 b, float t) {
    return a + (b - a) * t;
}

// Standard error bound for the midpoint quadratic approximation:
// (sqrt(3)/36) * ||p3 - 3 p2 + 3 p1 - p0||.
inline float approx_error(const Cubic& c) {
    kernel::cfloat2 d = c.p3 - c.p2 * 3.0f + c.p1 * 3.0f - c.p0;
    return 0.0481125f * kernel::clength(d);
}

inline void split(const Cubic& c, Cubic& l, Cubic& r) {
    using kernel::cfloat2;
    cfloat2 p01 = lerp2(c.p0, c.p1, 0.5f);
    cfloat2 p12 = lerp2(c.p1, c.p2, 0.5f);
    cfloat2 p23 = lerp2(c.p2, c.p3, 0.5f);
    cfloat2 p012 = lerp2(p01, p12, 0.5f);
    cfloat2 p123 = lerp2(p12, p23, 0.5f);
    cfloat2 mid = lerp2(p012, p123, 0.5f);
    l = Cubic{c.p0, p01, p012, mid};
    r = Cubic{mid, p123, p23, c.p3};
}

// Midpoint quadratic approximation of a (sufficiently flat) cubic.
inline QuadBezier to_quadratic(const Cubic& c) {
    using kernel::cfloat2;
    cfloat2 ctrl = (c.p1 + c.p2) * 3.0f - (c.p0 + c.p3);
    return QuadBezier{c.p0, ctrl * 0.25f, c.p3};
}

}  // namespace detail

// Adaptively subdivide a cubic Bézier into quadratics whose deviation from
// the cubic stays within `tolerance` (world units). Depth-bounded, iterative
// (explicit stack), deterministic.
inline std::vector<QuadBezier> cubic_to_quadratics(kernel::cfloat2 p0, kernel::cfloat2 p1,
                                                   kernel::cfloat2 p2, kernel::cfloat2 p3,
                                                   float tolerance, int max_depth = 10) {
    std::vector<QuadBezier> out;
    struct Item {
        detail::Cubic c;
        int depth;
    };
    std::vector<Item> stack;
    stack.push_back({detail::Cubic{p0, p1, p2, p3}, 0});
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        if (it.depth >= max_depth || detail::approx_error(it.c) <= tolerance) {
            out.push_back(detail::to_quadratic(it.c));
            continue;
        }
        detail::Cubic l, r;
        detail::split(it.c, l, r);
        stack.push_back({r, it.depth + 1});
        stack.push_back({l, it.depth + 1});
    }
    // Depth-first left-to-right traversal: restore curve order.
    return out;
}

// Unsigned distance to a cubic via its quadratic chain.
inline float sd_cubic_bezier(kernel::cfloat2 p, kernel::cfloat2 p0, kernel::cfloat2 p1,
                             kernel::cfloat2 p2, kernel::cfloat2 p3, float tolerance) {
    float d = 3.4e38f;
    for (const QuadBezier& q : cubic_to_quadratics(p0, p1, p2, p3, tolerance)) {
        d = kernel::cmin(d, kernel::sd_bezier2(p, q.a, q.b, q.c));
    }
    return d;
}

}  // namespace math
}  // namespace clay
