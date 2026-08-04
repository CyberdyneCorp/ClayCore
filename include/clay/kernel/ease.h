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
CLAY_FN float cease_in_circ(float t) { return 1.0f - csqrt(1.0f - t * t); }
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
