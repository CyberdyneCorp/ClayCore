// Every easing curve's declared maximum slope is at or above its real one.
//
// WHY THIS FILE EXISTS. `ease_max_slope` feeds `cfi_grab`, which multiplies a
// link's Lipschitz by `1 + |d| * slope / r`, and a deformer chain multiplies
// those factors together. So the slope is raised to the power of the chain
// length, and an error in it is amplified accordingly:
//
//   a blanket 1.25x margin on EVERY curve, which is what this used to return,
//   made the declared bound 12.44x too large at 48 grabs -- 1,443,068 against
//   an analytic 116,008 (#542). 5.4% per link became 12.4x.
//
// THE DIRECTION OF THE ERROR IS NOT SYMMETRIC, which is the whole reason this
// test is strict in one direction and loose in the other:
//
//   declared TOO LARGE   the marcher takes more steps than it needs. Frames.
//   declared TOO SMALL   the marcher steps THROUGH the surface. Geometry, and
//                        silently: holes, missed picks, a sculpt that renders
//                        wrong rather than slowly.
//
// So the load-bearing assertion is `declared >= observed`, on every curve,
// with no tolerance in the unsafe direction. The tightness assertions exist
// only to stop the analytic values silently regressing back to a blanket
// margin, and they are deliberately generous.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "clay/kernel/ease.h"
#include "clay/scene/bounds.h"

using namespace clay;

namespace {

// The largest |E(t+h) - E(t)| / h found by a dense forward sweep. A sampled
// maximum is a LOWER bound on the true supremum, which is the safe direction
// here: if the declared value clears this it may still be below the true
// supremum, so this test proves a necessary condition and not a sufficient one.
// That is stated rather than hidden, and it is why the curves whose suprema are
// not known analytically keep their sampled margin.
// THE SAMPLE DENSITY IS BOUNDED BY FLOAT PRECISION, not by patience.
// `cease` returns float, so differencing two of its values carries about 1.2e-7
// of rounding, and dividing by h amplifies that to 1.2e-7/h. At h = 1/65536
// that is 8e-3 -- which is precisely the 0.0073 by which a first version of this
// test "measured" the sine curves ABOVE their true supremum of pi/2 and failed
// a correct bound. Denser is not more accurate here past a point; it is louder.
//
// At 4096 samples the noise floor is about 5e-4, so kNoiseAllowance is set an
// order of magnitude above it and still far below any real discrepancy.
constexpr int kSamples = 4096;
constexpr double kNoiseAllowance = 5e-3;

double observed_max_slope(std::uint8_t ease, int samples) {
    double worst = 0.0;
    double prev = kernel::cease(ease, 0.0f);
    for (int i = 1; i <= samples; ++i) {
        const double t = double(i) / double(samples);
        const double v = kernel::cease(ease, float(t));
        worst = std::max(worst, std::fabs(v - prev) * double(samples));
        prev = v;
    }
    return worst;
}

}  // namespace

TEST_CASE("every easing's declared slope is at or above its measured one") {
    // Eight times the density the declaration itself uses, so a curve whose
    // steepest segment sits between two of its 512 points has a chance of being
    // caught -- bounded above by the float noise floor documented at kSamples.
    for (int e = 0; e < kernel::ease_count; ++e) {
        // NOTHING IS EXCLUDED. The circ family used to be, because its
        // derivative is unbounded at the endpoint and no sampled value can
        // bound an infinity -- it was pinned in its own case as a defect. The
        // curve is now held short of that singularity and renormalised (#543),
        // so every easing in the table is covered by this loop and a new one
        // that under-declares fails here rather than needing its own case.
        CAPTURE(e);
        const double declared = scene::ease_max_slope(std::uint8_t(e));
        const double observed = observed_max_slope(std::uint8_t(e), kSamples);
        // THE ASSERTION THAT MATTERS. The only tolerance is the measurement's own
        // float noise, quantified above -- never a tolerance on the bound.
        CHECK(declared >= observed - kNoiseAllowance);
    }
}

TEST_CASE("the analytic slopes are the real suprema, not a margin around them") {
    // Each of these is a closed-form maximum of |E'| on [0, 1]. If one drifts
    // the bound either stops being safe or silently reacquires the 1.25x that
    // #542 removed, and only this case would notice.
    struct Row {
        std::uint8_t ease;
        double slope;
        const char* why;
    };
    const Row rows[] = {
        {kernel::ease_linear, 1.0, "E(t) = t"},
        {kernel::ease_smoothstep, 1.5, "3t^2-2t^3, peak 3/2 at t=1/2"},
        {kernel::ease_smootherstep, 1.875, "6t^5-15t^4+10t^3, peak 15/8 at t=1/2"},
        {kernel::ease_in_quad, 2.0, "t^2, peak 2 at t=1"},
        {kernel::ease_out_quad, 2.0, "mirror of in_quad"},
        {kernel::ease_in_cubic, 3.0, "t^3, peak 3 at t=1"},
        {kernel::ease_out_cubic, 3.0, "mirror of in_cubic"},
        {kernel::ease_in_quart, 4.0, "t^4"},
        {kernel::ease_out_quart, 4.0, "mirror"},
        {kernel::ease_in_quint, 5.0, "t^5"},
        {kernel::ease_out_quint, 5.0, "mirror"},
        {kernel::ease_in_out_quad, 2.0, "2^(n-1) t^n on the first half; E' peaks at n"},
        {kernel::ease_in_out_cubic, 3.0, "peaks at n, not 2^(n-1) n"},
        {kernel::ease_in_out_quart, 4.0, "peaks at n"},
        {kernel::ease_in_out_quint, 5.0, "peaks at n"},
        {kernel::ease_in_sine, 1.5707964, "pi/2"},
        {kernel::ease_out_sine, 1.5707964, "pi/2"},
        {kernel::ease_in_out_sine, 1.5707964, "pi/2"},
    };
    for (const Row& r : rows) {
        CAPTURE(r.why);
        const double declared = scene::ease_max_slope(r.ease);
        const double observed = observed_max_slope(r.ease, kSamples);
        // Safe: never below the truth, allowing only for the sampler's noise.
        CHECK(declared >= observed - kNoiseAllowance);
        // Tight: the declared value IS the analytic supremum, not a margin
        // around it. A dense sample approaches the supremum from below, so it
        // should land just under the declared figure and nowhere near 1.25x it.
        CHECK(declared == doctest::Approx(r.slope).epsilon(1e-6));
        CHECK(observed > declared * 0.98);
    }
}

TEST_CASE("linear is exactly 1, which is the case that compounds") {
    // ease_linear is the default and the only value ClaySpaceDesktop has ever
    // sent, so it is the one that decides what a real Move chain declares. It
    // returned 1.25 before #542, and 1.25^48 against 1.0^48 is the entire 12.4x.
    CHECK(scene::ease_max_slope(kernel::ease_linear) == doctest::Approx(1.0f));

    // What that is worth on a chain, stated as the arithmetic rather than as a
    // claim: a link contributing (1 + |d| * slope / r) with |d|/r = 0.275.
    const double exact = 1.0 + 0.275 * 1.0;
    const double old_margin = 1.0 + 0.275 * 1.25;
    double compounded = 1.0;
    for (int i = 0; i < 48; ++i) compounded *= old_margin / exact;
    CHECK(compounded > 10.0);   // the 12.4x measured in #542
    CHECK(compounded < 15.0);
}

TEST_CASE("the circ family is bounded, because the curve is held short of its singularity") {
    // WAS A PRE-EXISTING SOUNDNESS DEFECT (issue #543), pinned here as failing
    // until it was fixed. E(t) = 1 - sqrt(1 - t^2) has E'(t) = t/sqrt(1 - t^2),
    // UNBOUNDED as t -> 1, so no sampled value could bound it: the declared
    // slope came back at 39.98 where a 4096-point sweep measured 90.50, and a
    // denser sweep found more again. A bound BELOW the truth lets the marcher
    // step through the surface.
    //
    // The curve is now held at 1 - CLAY_CIRC_GUARD and renormalised, so its
    // supremum is finite, closed form, and reached exactly at the guard --
    // and ease_max_slope computes it from the SAME constant, so the two
    // cannot drift apart.
    for (const std::uint8_t e : {static_cast<std::uint8_t>(kernel::ease_in_circ),
                                 static_cast<std::uint8_t>(kernel::ease_out_circ),
                                 static_cast<std::uint8_t>(kernel::ease_in_out_circ)}) {
        CAPTURE(int(e));
        const double declared = scene::ease_max_slope(e);
        const double observed = observed_max_slope(e, kSamples);
        CHECK(declared >= observed - kNoiseAllowance);
    }

    // AND THE CURVE STILL JOINS ITSELF. Clamping alone left f(1) at 0.98586,
    // and CLAY_EASE_INOUT joins 0.5*f(2t) to 1 - 0.5*f(2-2t) at t = 0.5 -- so
    // the halves stopped meeting and the curve jumped by 0.0141 there. A
    // discontinuity is worse than a steep slope, and a difference quotient
    // across it measured 92.96 against an analytic 70.70. This is the
    // assertion that caught it.
    const double lo = kernel::cease(kernel::ease_in_out_circ, 0.5f - 1e-4f);
    const double hi = kernel::cease(kernel::ease_in_out_circ, 0.5f + 1e-4f);
    CAPTURE(lo);
    CAPTURE(hi);
    CHECK(std::fabs(hi - lo) < 1e-2);

    // AND THE CURVE ITSELF IS PINNED, not only its slope.
    //
    // NO PARITY SCENE OR DEFORMER GOLDEN USES A CIRC EASING -- checked, and the
    // reason the whole suite stayed green through a change to kernel math. So
    // these values are the only thing standing between the guard and a silent
    // reshaping of the curve, and they are here rather than in a golden file
    // because the guard is what they exist to pin.
    //
    // Taken from the renormalised curve at CLAY_CIRC_GUARD = 1e-4. Changing the
    // guard SHOULD fail these: it is a deliberate reshaping and wants to be
    // seen, not absorbed.
    CHECK(kernel::cease(kernel::ease_in_circ, 0.25f) == doctest::Approx(0.0328).epsilon(0.01));
    CHECK(kernel::cease(kernel::ease_in_circ, 0.50f) == doctest::Approx(0.1353).epsilon(0.01));
    CHECK(kernel::cease(kernel::ease_in_circ, 0.90f) == doctest::Approx(0.5701).epsilon(0.01));
    CHECK(kernel::cease(kernel::ease_in_circ, 0.99f) == doctest::Approx(0.8576).epsilon(0.01));

    // The endpoints are exact, which is what renormalising buys: a grab's
    // centre takes the FULL displacement rather than 98.6% of it.
    CHECK(kernel::cease(kernel::ease_in_circ, 1.0f) == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(kernel::cease(kernel::ease_in_circ, 0.0f) == doctest::Approx(0.0).epsilon(1e-5));
}

TEST_CASE("the curves that are still sampled keep a margin, and are named") {
    // circ has an infinite derivative at its endpoint, expo is stiff at one
    // end, and back/elastic/bounce overshoot outside [0,1]. None of their
    // suprema are declared analytically here, so each must still come back
    // strictly above a dense sample -- the margin is doing real work for them.
    const std::uint8_t sampled[] = {
        kernel::ease_in_expo,     kernel::ease_out_expo,     kernel::ease_in_out_expo,
        kernel::ease_in_back,     kernel::ease_out_back,     kernel::ease_in_out_back,
        kernel::ease_in_elastic,  kernel::ease_out_elastic,  kernel::ease_in_out_elastic,
        kernel::ease_in_bounce,   kernel::ease_out_bounce,   kernel::ease_in_out_bounce,
    };
    for (const std::uint8_t e : sampled) {
        CAPTURE(int(e));
        const double declared = scene::ease_max_slope(e);
        const double observed = observed_max_slope(e, kSamples);
        CHECK(declared >= observed - kNoiseAllowance);
        // Strictly above, not equal: these have no closed form here and the
        // margin is the only thing standing between a 512-point sample and a
        // steeper segment it missed.
        CHECK(declared > observed - kNoiseAllowance);
    }
}
