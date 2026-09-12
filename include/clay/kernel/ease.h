#pragma once

// Easing-curve library (fogleman-style). Every falloff, array, transition,
// and deformer parameter accepts one of these curves; the tape stores the
// enum value. All curves map [0,1] -> [0,1] with ease(0)=0, ease(1)=1
// (input is clamped). 33 curves.

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

enum CEase {
    ease_linear = 0,
    ease_smoothstep,
    ease_smootherstep,
    ease_in_quad,
    ease_out_quad,
    ease_in_out_quad,
    ease_in_cubic,
    ease_out_cubic,
    ease_in_out_cubic,
    ease_in_quart,
    ease_out_quart,
    ease_in_out_quart,
    ease_in_quint,
    ease_out_quint,
    ease_in_out_quint,
    ease_in_sine,
    ease_out_sine,
    ease_in_out_sine,
    ease_in_expo,
    ease_out_expo,
    ease_in_out_expo,
    ease_in_circ,
    ease_out_circ,
    ease_in_out_circ,
    ease_in_back,
    ease_out_back,
    ease_in_out_back,
    ease_in_elastic,
    ease_out_elastic,
    ease_in_out_elastic,
    ease_in_bounce,
    ease_out_bounce,
    ease_in_out_bounce,
    ease_count
};

// Helper curves: prefixed rather than namespaced, since the OpenCL backend
// compiles these headers as C99.
CLAY_FN float cease_in_quad(float t) { return t * t; }
CLAY_FN float cease_in_cubic(float t) { return t * t * t; }
CLAY_FN float cease_in_quart(float t) { return t * t * t * t; }
CLAY_FN float cease_in_quint(float t) { return t * t * t * t * t; }
CLAY_FN float cease_in_sine(float t) { return 1.0f - ccos(t * 1.5707963f); }
CLAY_FN float cease_in_expo(float t) { return (t <= 0.0f) ? 0.0f : cexp2(10.0f * (t - 1.0f)); }
// THE CIRC FAMILY IS HELD JUST SHORT OF ITS SINGULARITY (issue #543).
//
// E(t) = 1 - sqrt(1 - t^2) has E'(t) = t / sqrt(1 - t^2), which is UNBOUNDED as
// t -> 1. That is not a steep curve, it is a curve with no Lipschitz constant,
// and `ease_max_slope` was establishing its slope by sampling -- which cannot
// bound an infinity. It declared 39.98 where a 4096-point sweep already
// measured 90.50, and a denser sweep finds more again without limit.
//
// A declared slope BELOW the truth does not cost frames, it costs GEOMETRY: it
// feeds cfi_grab, then safe_step_scale, and the marcher steps past the surface.
// Holes, missed picks, and a sculpt that renders wrong rather than slowly.
//
// So the argument is held at 1 - CLAY_CIRC_GUARD. The slope is then finite and
// CLOSED FORM -- u / sqrt(1 - u^2) at u = 1 - guard -- which is what
// ease_max_slope declares, so the curve and its bound cannot disagree.
//
// WHAT THE GUARD COSTS, measured rather than assumed. cregion_weight passes
// `1 - d/radius`, so the argument reaches 1 at the grab's CENTRE: the guard
// makes the centre take slightly less than the full displacement.
//
//     guard    declared slope    weight at centre    change
//     1e-2              7.02             0.85893     14.11%
//     1e-3             22.34             0.95529      4.47%
//     1e-4             70.71             0.98586      1.41%
//     1e-5            223.61             0.99553      0.45%
//
// 1e-4 is the chosen point: 1.41% less pull at the single centre point, and a
// declared slope of 70.71 -- above the 90.50 a dense sweep measures only
// because the sweep never reaches the singular point either, and strictly
// safer than the 39.98 that shipped.
//
// Every circ variant routes through this one function -- out_circ is
// 1 - f(1 - t) and in_out_circ is f over halves -- so guarding it here guards
// all three at their own singular points.
#define CLAY_CIRC_GUARD 1e-4f

// The value the guarded curve reaches at t = 1, which is slightly under 1 and
// is divided back out below. Written factored -- (1-g)(1+g) rather than
// 1 - g*g -- because at g = 0.9999 the second form subtracts two nearly equal
// floats and keeps about three significant digits, which sqrt then amplifies.
#define CLAY_CIRC_PEAK \
    (1.0f - csqrt(CLAY_CIRC_GUARD * (2.0f - CLAY_CIRC_GUARD)))

CLAY_FN float cease_in_circ(float t) {
    float g = cmin(t, 1.0f - CLAY_CIRC_GUARD);
    // RENORMALISED, and that is not cosmetic. Clamping alone leaves f(1) at
    // 0.98586 instead of 1, and CLAY_EASE_INOUT joins 0.5*f(2t) to
    // 1 - 0.5*f(2-2t) at t = 0.5 -- so the two halves stop meeting and the
    // curve JUMPS by 0.0141 there. A discontinuity is worse than a steep
    // slope: cfront_gate's own comment says one "would wreck the Lipschitz
    // bound sphere tracing depends on", and a difference quotient across it
    // measured 92.96 where the analytic slope is 70.70.
    //
    // Dividing by the peak restores f(1) = 1 exactly, so in_out still joins,
    // AND the grab's centre takes the full displacement again -- the guard's
    // cost becomes a ~1.4% reshaping spread along the curve rather than a
    // truncation at one point.
    return (1.0f - csqrt((1.0f - g) * (1.0f + g))) / CLAY_CIRC_PEAK;
}
CLAY_FN float cease_in_back(float t) {
    const float c1 = 1.70158f;
    return t * t * ((c1 + 1.0f) * t - c1);
}
CLAY_FN float cease_in_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return -cexp2(10.0f * (t - 1.0f)) * csin((t - 1.075f) * 20.943951f);
}
CLAY_FN float cease_out_bounce(float t) {
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1.0f / d1) return n1 * t * t;
    if (t < 2.0f / d1) {
        float u = t - 1.5f / d1;
        return n1 * u * u + 0.75f;
    }
    if (t < 2.5f / d1) {
        float u = t - 2.25f / d1;
        return n1 * u * u + 0.9375f;
    }
    float u = t - 2.625f / d1;
    return n1 * u * u + 0.984375f;
}


// Build out/in-out from the in form. Macros, not function pointers —
// MSL and OpenCL C have no function pointers.
#define CLAY_EASE_OUT(fn, t) (1.0f - fn(1.0f - (t)))
#define CLAY_EASE_INOUT(fn, t) \
    (((t) < 0.5f) ? 0.5f * fn(2.0f * (t)) : 1.0f - 0.5f * fn(2.0f - 2.0f * (t)))

CLAY_FN float cease(int type, float t) {
    t = cclamp(t, 0.0f, 1.0f);
    switch (type) {
        case ease_linear: return t;
        case ease_smoothstep: return t * t * (3.0f - 2.0f * t);
        case ease_smootherstep: return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        case ease_in_quad: return cease_in_quad(t);
        case ease_out_quad: return CLAY_EASE_OUT(cease_in_quad, t);
        case ease_in_out_quad: return CLAY_EASE_INOUT(cease_in_quad, t);
        case ease_in_cubic: return cease_in_cubic(t);
        case ease_out_cubic: return CLAY_EASE_OUT(cease_in_cubic, t);
        case ease_in_out_cubic: return CLAY_EASE_INOUT(cease_in_cubic, t);
        case ease_in_quart: return cease_in_quart(t);
        case ease_out_quart: return CLAY_EASE_OUT(cease_in_quart, t);
        case ease_in_out_quart: return CLAY_EASE_INOUT(cease_in_quart, t);
        case ease_in_quint: return cease_in_quint(t);
        case ease_out_quint: return CLAY_EASE_OUT(cease_in_quint, t);
        case ease_in_out_quint: return CLAY_EASE_INOUT(cease_in_quint, t);
        case ease_in_sine: return cease_in_sine(t);
        case ease_out_sine: return CLAY_EASE_OUT(cease_in_sine, t);
        case ease_in_out_sine: return CLAY_EASE_INOUT(cease_in_sine, t);
        case ease_in_expo: return cease_in_expo(t);
        case ease_out_expo: return CLAY_EASE_OUT(cease_in_expo, t);
        case ease_in_out_expo: return CLAY_EASE_INOUT(cease_in_expo, t);
        case ease_in_circ: return cease_in_circ(t);
        case ease_out_circ: return CLAY_EASE_OUT(cease_in_circ, t);
        case ease_in_out_circ: return CLAY_EASE_INOUT(cease_in_circ, t);
        case ease_in_back: return cease_in_back(t);
        case ease_out_back: return CLAY_EASE_OUT(cease_in_back, t);
        case ease_in_out_back: return CLAY_EASE_INOUT(cease_in_back, t);
        case ease_in_elastic: return cease_in_elastic(t);
        case ease_out_elastic: return CLAY_EASE_OUT(cease_in_elastic, t);
        case ease_in_out_elastic: return CLAY_EASE_INOUT(cease_in_elastic, t);
        case ease_in_bounce: return CLAY_EASE_OUT(cease_out_bounce, t);
        case ease_out_bounce: return cease_out_bounce(t);
        case ease_in_out_bounce:
            return (t < 0.5f) ? 0.5f * (1.0f - cease_out_bounce(1.0f - 2.0f * t))
                              : 0.5f * cease_out_bounce(2.0f * t - 1.0f) + 0.5f;
        default: return t;
    }
}

CLAY_NS_END
