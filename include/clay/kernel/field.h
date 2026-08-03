#pragma once

// Field utilities (docs/01 §3): tetrahedron-trick normals, sphere tracing
// with over-relaxation and pixel-proportional epsilon, AO, Aaltonen soft
// shadows. Templated over the field functor F: cfloat3 -> float (MSL and
// CUDA accept templates; the OpenCL C subset gets macro instantiations with
// the phase-4 backend).
//
// step_scale = csafe_step_scale(tree CFieldInfo) — callers MUST pass the
// tracked value, never assume 1 (sdf-kernels spec).

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

// Tetrahedron trick: 4 taps, symmetric error cancellation. h scales with
// pixel footprint.
template <typename F>
CLAY_FN cfloat3 cnormal(F f, cfloat3 p, float h) {
    const cfloat3 e0 = cf3(1.0f, -1.0f, -1.0f);
    const cfloat3 e1 = cf3(-1.0f, -1.0f, 1.0f);
    const cfloat3 e2 = cf3(-1.0f, 1.0f, -1.0f);
    const cfloat3 e3 = cf3(1.0f, 1.0f, 1.0f);
    cfloat3 n = e0 * f(p + e0 * h) + e1 * f(p + e1 * h) + e2 * f(p + e2 * h) + e3 * f(p + e3 * h);
    return cnormalize(n);
}

struct CRayHit {
    bool hit;
    float t;      // ray parameter of the hit (refined)
    int steps;    // iterations used
};

// Sphere tracing (Hart 1996) with Keinert over-relaxation and a
// pixel-proportional epsilon (eps is the tangent of the pixel half-angle;
// the acceptance threshold is eps * t). relax in [1, 2); 1 disables
// over-relaxation. On acceptance the hit t is refined by linear
// interpolation between the last two samples.
template <typename F>
CLAY_FN CRayHit craycast(F f, cfloat3 ro, cfloat3 rd, float tmin, float tmax, float eps,
                         float step_scale, float relax, int max_steps) {
    CRayHit result;
    result.hit = false;
    result.t = tmax;
    result.steps = 0;

    float t = tmin;
    float omega = relax;
    float prev_radius = 0.0f;
    float step_len = 0.0f;
    float prev_t = tmin;
    float prev_h = 3.4e38f;

    for (int i = 0; i < max_steps && t < tmax; ++i) {
        result.steps = i + 1;
        float h = f(ro + rd * t) * step_scale;
        float radius = cabs(h);

        bool sor_fail = omega > 1.0f && (radius + prev_radius) < step_len;
        if (sor_fail) {
            // Unbounding spheres no longer overlap: retreat and go safe.
            t -= step_len;
            omega = 1.0f;
            step_len = 0.0f;
            continue;
        }

        if (radius < eps * cmax(t, tmin)) {
            // Refine with the last two samples when they bracket or descend.
            float denom = h - prev_h;
            if (prev_h < 3.4e38f && cabs(denom) > 1e-12f) {
                float ft = cclamp(-prev_h / denom, 0.0f, 1.0f);
                result.t = cmix(prev_t, t, ft);
            } else {
                result.t = t;
            }
            result.hit = true;
            return result;
        }

        prev_t = t;
        prev_h = h;
        prev_radius = radius;
        step_len = h * omega;
        t += step_len;
    }
    return result;
}

// Ambient occlusion: 5 taps along the normal; shortfall = occlusion.
template <typename F>
CLAY_FN float cao(F f, cfloat3 pos, cfloat3 nor) {
    float occ = 0.0f;
    float sca = 1.0f;
    for (int i = 0; i < 5; ++i) {
        float h = 0.01f + 0.12f * (float)i / 4.0f;
        occ += (h - f(pos + nor * h)) * sca;
        sca *= 0.95f;
    }
    return cclamp(1.0f - 3.0f * occ, 0.0f, 1.0f);
}

// Aaltonen soft shadow (triangulation kills banding). w = light size.
template <typename F>
CLAY_FN float csoftshadow(F f, cfloat3 ro, cfloat3 rd, float mint, float maxt, float w,
                          float step_scale) {
    float res = 1.0f;
    float ph = 1e20f;
    float t = mint;
    for (int i = 0; i < 256 && t < maxt; ++i) {
        float h = f(ro + rd * t) * step_scale;
        if (h < 0.001f) return 0.0f;
        // Triangulate only while the surface is approaching; on receding
        // rays (h >= ph) the estimate degenerates (y -> h, d -> 0) and the
        // plain h/(w*t) penumbra estimate is the safe one.
        float y = (h < ph) ? h * h / (2.0f * ph) : 0.0f;
        float d = csqrt(cmax(h * h - y * y, 0.0f));
        res = cmin(res, d / (w * cmax(t - y, 1e-6f)));
        ph = h;
        t += h;
    }
    return cclamp(res, 0.0f, 1.0f);
}

CLAY_NS_END
