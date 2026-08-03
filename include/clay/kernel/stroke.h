#pragma once

// Sculpt stroke item: a polyline chain of sphere-swept cones with per-point
// radius (Dreams-style Pencil smear). One stroke is ONE tape item — segments
// are combined here, not as separate scene items (sdf-kernels spec).
// Per-point color is sampled by the caller from the returned segment index
// and parameter. Mirror application happens at the scene level (evaluate the
// stroke at the mirrored point too and combine).

#include "clay/kernel/shim.h"
#include "clay/kernel/ops.h"
#include "clay/kernel/prim3d.h"

CLAY_NS_BEGIN

struct CStrokePoint {
    cfloat3 pos;
    float radius;
};

// Distance to the chain. blend_k = 0 gives the hard union of segments;
// blend_k > 0 smooth-unions consecutive results (quadratic profile).
// *out_seg / *out_t identify the closest segment and the projection
// parameter along it, for color/radius attribution.
CLAY_FN float sd_stroke(CLAY_DEVICE const CStrokePoint* pts, int count, cfloat3 p, float blend_k,
                        CLAY_THREAD int* out_seg, CLAY_THREAD float* out_t) {
    *out_seg = 0;
    *out_t = 0.0f;
    if (count <= 0) return 3.4e38f;
    if (count == 1) return sd_sphere(p - pts[0].pos, pts[0].radius);

    float d = 3.4e38f;
    float best = 3.4e38f;
    for (int i = 0; i + 1 < count; ++i) {
        cfloat3 a = pts[i].pos, b = pts[i + 1].pos;
        float seg_d;
        if (cdot2(b - a) < 1e-12f) {
            seg_d = sd_sphere(p - a, cmax(pts[i].radius, pts[i + 1].radius));
        } else if (cabs(pts[i].radius - pts[i + 1].radius) < 1e-7f) {
            seg_d = sd_capsule(p, a, b, pts[i].radius);
        } else {
            seg_d = sd_round_cone_ab(p, a, b, pts[i].radius, pts[i + 1].radius);
        }
        if (seg_d < best) {
            best = seg_d;
            *out_seg = i;
            cfloat3 ab = b - a;
            *out_t = cclamp(cdot(p - a, ab) / cmax(cdot(ab, ab), 1e-12f), 0.0f, 1.0f);
        }
        d = (blend_k > 0.0f) ? csmin_quadratic(d, seg_d, blend_k) : cmin(d, seg_d);
    }
    return d;
}

CLAY_NS_END
