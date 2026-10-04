#pragma once

// The steepest slope an easing curve reaches, on the host.
//
// Lives in math rather than scene because two layers need it and only one of
// them may include scene: the tape compiler and the region-deformer bounds
// (scene) multiply a link's Lipschitz factor by it, and Move Topological
// (field, which sits below scene) uses it to decide how many sub-steps a drag
// needs before its pull-back stops being one-to-one (#657). One definition, so
// the two cannot disagree about how steep a curve is.
//
// Host-only: it is not part of the kernel dialect, and no GPU path asks it.

#include <cmath>
#include <cstdint>

#include "clay/kernel/ease.h"

namespace clay {
namespace math {

// The steepest |E'(t)| an easing curve reaches on [0, 1].
//
// THIS NUMBER COMPOUNDS. It multiplies a link's Lipschitz factor
// (`cfi_grab`: 1 + |d| * slope / r), and a chain multiplies those factors, so
// an error here is raised to the power of the chain length. Measured: the
// blanket 1.25x margin this used to return for EVERY curve made a declared
// bound 12.44x too large at 48 grabs, against an analytic product of 116,008
// and a declared 1,443,068 (issue #542). A 5.4% per-link excess became 12.4x.
//
// So the curves whose derivative maximum is KNOWN return it exactly, and only
// the ones that are not return a sampled value with a margin.
//
// THE MARGIN IS NOT DECORATION and must stay where sampling stays: 512 samples
// of an arbitrary curve can miss a steeper segment between two of them, and a
// slope that is too SMALL under-declares the bound, which does not make the
// marcher slow -- it makes it step through the surface. Too large costs frames;
// too small costs geometry. These constants are only safe because each is the
// exact supremum of |E'| for its curve, checked against a dense sample in
// test_ease_slopes.cpp.
inline float ease_max_slope(std::uint8_t ease) {
    switch (ease) {
        // E(t) = t.
        case kernel::ease_linear: return 1.0f;
        // 3t^2 - 2t^3; E' peaks at t = 1/2 with 3/2.
        case kernel::ease_smoothstep: return 1.5f;
        // 6t^5 - 15t^4 + 10t^3; E' peaks at t = 1/2 with 15/8.
        case kernel::ease_smootherstep: return 1.875f;
        // t^n and its mirror peak at the endpoint with n.
        case kernel::ease_in_quad:
        case kernel::ease_out_quad: return 2.0f;
        case kernel::ease_in_cubic:
        case kernel::ease_out_cubic: return 3.0f;
        case kernel::ease_in_quart:
        case kernel::ease_out_quart: return 4.0f;
        case kernel::ease_in_quint:
        case kernel::ease_out_quint: return 5.0f;
        // The in-out forms are 2^(n-1) t^n on the first half, so E' peaks at
        // the midpoint with n * 2^(n-1) * (1/2)^(n-1) = n -- the SAME slope as
        // the one-sided forms, not 2^(n-1) times it. A first version of this
        // wrote 2/4/8/16 while the comment beside it derived n correctly;
        // test_ease_slopes.cpp caught it because a dense sample of in_out_quart
        // reads 4.0 against a declared 8.0.
        case kernel::ease_in_out_quad: return 2.0f;
        case kernel::ease_in_out_cubic: return 3.0f;
        case kernel::ease_in_out_quart: return 4.0f;
        case kernel::ease_in_out_quint: return 5.0f;
        // sin/cos quarter turns: the derivative peaks at pi/2.
        case kernel::ease_in_sine:
        case kernel::ease_out_sine: return 1.5707964f;          // pi/2
        case kernel::ease_in_out_sine: return 1.5707964f;       // pi/2
        default: break;
    }
    // THE CIRC FAMILY IS NOW CLOSED FORM, because the curve itself is held
    // short of its singularity (issue #543, CLAY_CIRC_GUARD in kernel/ease.h).
    //
    // Its derivative is u / sqrt(1 - u^2), unbounded as u -> 1, so no sampled
    // value could ever bound it -- this declared 39.98 where a 4096-point sweep
    // measured 90.50, and a denser sweep finds more again. A slope BELOW the
    // truth does not cost frames, it costs geometry.
    //
    // With the argument guarded the supremum is reached exactly at the guard
    // and is analytic. Computed from the same constant the curve uses, so the
    // two cannot drift apart: change the guard and this follows.
    switch (ease) {
        case kernel::ease_in_circ:
        case kernel::ease_out_circ:
        case kernel::ease_in_out_circ: {
            // u / sqrt(1-u^2) at the guard, divided by the same peak the
            // curve is renormalised by, so the bound tracks the curve exactly.
            const float u = 1.0f - CLAY_CIRC_GUARD;
            const float peak =
                1.0f - std::sqrt(CLAY_CIRC_GUARD * (2.0f - CLAY_CIRC_GUARD));
            return u / (std::sqrt((1.0f - u) * (1.0f + u)) * peak);
        }
        default: break;
    }
    // NOT known analytically here, and deliberately left sampled: expo is stiff
    // near one end, and back / elastic / bounce overshoot outside [0, 1] -- the
    // undershoot that #527 had to stop being deleted. Their suprema deserve
    // their own derivation and their own tests; until then they keep the
    // sampled value and the margin that makes it safe.
    const int kSamples = 512;
    float worst = 1.0f;
    float prev = kernel::cease(ease, 0.0f);
    for (int i = 1; i <= kSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSamples);
        float v = kernel::cease(ease, t);
        worst = kernel::cmax(worst, kernel::cabs(v - prev) * static_cast<float>(kSamples));
        prev = v;
    }
    return worst * 1.25f;  // sampling headroom
}

}  // namespace math
}  // namespace clay
