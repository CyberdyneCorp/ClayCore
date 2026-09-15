// The finite-difference step a surface measure is taken with (issue #596).
//
// WHY THIS FILE EXISTS. `clay_measure_params.h` is documented as "Finite-
// difference step for the Laplacian and the normal, in world units. 0 derives
// one from `scale`. Smaller measures noise; larger blurs the feature being
// measured." Nothing asserted that it does any of that.
//
// The three places that set it non-default all assert that two paths AGREE with
// each other -- tests/unit/test_mesh_sculpt.cpp:2019 says so outright, "asking
// it and asking `measure_at` directly must give the same number. Not close --
// the same call." That is a wiring check, and a wiring check is A-vs-A: both
// sides move together when the parameter is dropped, so both stay equal and the
// test stays green.
//
// Measured rather than assumed: replacing the caller's `h` with the derived
// default -- the exact shape of the #593 defect, a field silently ignored --
// fails ZERO of the suite's 16,633,178 assertions.
//
// WHAT THIS ASSERTS INSTEAD. `h` controls a length scale, so the fixture is a
// surface with a known length in it: a plane rippled at a known wavelength. A
// central second difference of sin(kx) with step h is exactly
//
//     -sin(kx) * 2(1 - cos(kh)) / h^2
//
// which tends to the true k^2 as h shrinks and falls to EXACTLY ZERO at h = the
// wavelength, where the stencil spans a whole period and the feature averages
// out of existence. That is "larger blurs the feature" as an equation, so this
// file checks the curve rather than merely that something changed.
//
// TWO SIZING TRAPS, both of which cost a wrong answer here before the fixture
// worked. The reading is `saturate(|laplacian * scale|)`, so an amplitude large
// enough to be obvious pins it at 1.0 for EVERY h and the effect vanishes into
// the ceiling. And the amplitude also has to stay small enough that the field
// is still a distance function -- A*k here is 0.063, so the gradient stays
// within a thousandth of unit length.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/brush/surface_measure.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

constexpr float kLambda = 0.05f;   // the feature's size
constexpr float kAmp = 0.0005f;    // small: keeps |grad| ~ 1 AND the reading off the ceiling
constexpr float kScale = 0.06f;
constexpr float kTwoPi = 6.2831853f;

float ripple(cfloat3 p) { return p.y - kAmp * std::sin(kTwoPi * p.x / kLambda); }

// A crest, where the curvature is extremal and the sign is steady.
cfloat3 crest() { return cf3(kLambda * 0.25f, kAmp, 0.0f); }

float curvature_at(float h) {
    brush::MeasureSettings s;
    s.scale = kScale;
    s.h = h;
    return brush::measure_at(ripple, brush::SurfaceMeasure::Curvature, crest(), s);
}

// What a central second difference of this ripple must read, from the closed
// form above. Not a copy of the implementation -- it is what a central
// difference IS, so an implementation that stopped being one would fail here.
double predicted(double h) {
    const double k = static_cast<double>(kTwoPi) / kLambda;
    const double second = 2.0 * (1.0 - std::cos(k * h)) / (h * h);
    return static_cast<double>(kAmp) * second * static_cast<double>(kScale);
}

}  // namespace

TEST_CASE("measure: the finite-difference step decides which features are seen") {
    SUBCASE("a step far below the feature resolves it") {
        // h = lambda/50. Close to the true k^2, which the closed form gives as
        // the limit.
        const float fine = curvature_at(kLambda / 50.0f);
        CAPTURE(fine);
        CHECK(fine > 0.40f);
        CHECK(fine == doctest::Approx(predicted(kLambda / 50.0f)).epsilon(1e-3));
    }

    SUBCASE("a step at the feature's own size blurs it out of existence") {
        // The stencil spans a whole period, so the three samples average to the
        // mean plane and the ripple is not there at all.
        const float blurred = curvature_at(kLambda);
        CAPTURE(blurred);
        CHECK(blurred < 0.01f);
    }

    SUBCASE("and it falls monotonically in between") {
        // The property a host relies on when it turns h into a slider: no
        // reversal, so a step that reads less detail never reads more.
        const std::vector<float> steps = {0.001f, 0.002f, 0.005f,  0.008f, 0.0125f,
                                          0.02f,  0.025f, 0.03f,   0.04f,  0.045f};
        float last = 2.0f;
        for (const float h : steps) {
            const float v = curvature_at(h);
            CAPTURE(h);
            CAPTURE(v);
            CHECK(v < last);
            CHECK(v == doctest::Approx(predicted(h)).epsilon(1e-3));
            last = v;
        }
    }

    SUBCASE("zero derives a step from scale rather than dividing by it") {
        // Documented as "0 derives one from `scale`". The test is that it lands
        // where an explicit step of that size lands -- a default that silently
        // meant something else would be the same class of defect as ignoring
        // the field entirely.
        brush::MeasureSettings s;
        s.scale = kScale;
        s.h = 0.0f;
        const float derived =
            brush::measure_at(ripple, brush::SurfaceMeasure::Curvature, crest(), s);
        CAPTURE(derived);
        CHECK(derived == doctest::Approx(curvature_at(kScale * 0.2f)).epsilon(1e-4));
        // And it must be a USABLE default on this fixture rather than one of
        // the degenerate ends: neither saturated nor blurred flat.
        CHECK(derived > 0.01f);
        CHECK(derived < 1.0f);
    }
}
