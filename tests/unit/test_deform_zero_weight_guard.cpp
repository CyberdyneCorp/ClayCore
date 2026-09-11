// The zero-weight early-out in the region deformers is `w == 0.0f`, and this
// file exists so that nobody can "simplify" it to `w <= 0.0f`.
//
// WHY THE OPERATOR IS LOAD-BEARING. `cregion_weight` runs its [0,1] falloff
// parameter through `cease`, and four of the 33 curves LEAVE [0,1] on the way:
// ease_in_back and ease_in_out_back undershoot, ease_in_elastic and
// ease_in_out_elastic ring below zero. `cease_in_back(t) = t^2((c1+1)t - c1)`
// with c1 = 1.70158 is negative for t < c1/(c1+1) = 0.6296, and t = 1 - d/r, so
// it is negative for d > 0.3704r — which is 1 - 0.3704^3 = 94.9% of the ball's
// VOLUME. The undershoot is not an edge case at the rim. It is nearly the whole
// region, and it IS the curve: a back easing that does not pull back first is
// just a cubic.
//
// So a negative weight is a value to honour, not a degenerate one to guard
// against. `w <= 0.0f` swallows it and turns the deformer off over most of its
// own support; `w == 0.0f` early-outs of exactly the samples that were going to
// be multiplied by zero anyway, and of nothing else.
//
// THIS WAS A SHIPPED BUG, not a hypothetical. `cmagnify_point` and
// `cblob_offset` both wrote `w <= 0.0f`. Magnify skipped its radial scale
// wherever the weight was negative and read as inert over 94.9% of the ball
// under ease_in_back; blob returned no offset at all where the amplitude should
// have been negative — i.e. exactly where the dab should have been eating in
// rather than swelling. Both now read `w == 0.0f`. `cgrab_point` gained the
// same early-out as a performance change and was written `==` from the start.
//
// WHY NOT JUST THE GOLDEN TABLE. `test_deformer_goldens.cpp` does pin these
// numerically, and its magnify/ease_in_back row moved 17 literals when the bug
// was fixed. But a golden row can be re-baselined: someone who flips the
// operator back, sees a red table and regenerates it has a green build and a
// reintroduced bug. The checks below cannot be re-baselined, because they
// assert the PROPERTY rather than the value — a `<=` build fails them with no
// table to regenerate. Both halves are wanted; this is the half with no escape
// hatch.
//
// THE EASINGS ARE NAMED ON PURPOSE. Under any of the other 29 curves the two
// operators are indistinguishable, so a test that swept "an easing" would pass
// 29 times out of 33 and agree with the person doing the simplifying.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "clay/kernel/deform.h"
#include "clay/kernel/ease.h"

using namespace clay::kernel;

namespace {

constexpr float kRadius = 1.0f;
const cfloat3 kCentre = cf3(1.0f, 0.0f, 0.0f);
// A drag with a component on every axis the probes use, so a grab that fails to
// act shows up as a difference and not as a coincidental zero.
const cfloat3 kDrag = cf3(0.5f, 0.25f, 0.0f);

// A probe at d = 0.625 from the centre: comfortably INSIDE the unit region (so
// finite support is not what is being tested) and past the d = 0.3704 sign
// change (so the weight there is negative under ease_in_back). Every coordinate
// is an exact eighth, as in the golden table, so the probe contributes no
// rounding of its own.
const cfloat3 kInside = kCentre + cf3(0.625f, 0.0f, 0.0f);

bool bit_equal(cfloat3 a, cfloat3 b) {
    return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 &&
           std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
           std::memcmp(&a.z, &b.z, sizeof(float)) == 0;
}

float distance(cfloat3 a, cfloat3 b) {
    const cfloat3 e = a - b;
    return clength(e);
}

// `cgrab_point` with NO early-out at all — the definition the shipped one has
// to agree with. Kept here rather than in the kernel because it is the oracle,
// not a variant anyone should call.
cfloat3 grab_reference(cfloat3 p, cfloat3 centre, float radius, cfloat3 displacement,
                       float front_only, int ease_type) {
    float w = cregion_weight(p, centre, radius, ease_type);
    if (front_only != 0.0f) w = w * cfront_gate(p, centre, radius, displacement);
    return p - displacement * w;
}

}  // namespace

TEST_CASE("region weight really does go negative inside the ball under back and elastic easings") {
    // The premise every other case here rests on, asserted rather than assumed:
    // if a future `cease` change stopped these curves undershooting, the checks
    // below would still pass while testing nothing, and this is the case that
    // would go red and say why.
    CHECK(cregion_weight(kInside, kCentre, kRadius, ease_in_back) < 0.0f);
    CHECK(cregion_weight(kInside, kCentre, kRadius, ease_in_elastic) > 0.0f);

    // ease_in_elastic rings, so its negative band is elsewhere in the region
    // rather than at the same probe. Find it rather than hard-coding a lobe of
    // sin(): the oscillation's phase is an implementation detail of `cease` and
    // pinning it here would make this file fail for an unrelated reason.
    int negative_samples = 0;
    for (int i = 1; i < 64; ++i) {
        const float d = kRadius * static_cast<float>(i) / 64.0f;
        if (cregion_weight(kCentre + cf3(d, 0.0f, 0.0f), kCentre, kRadius, ease_in_elastic) < 0.0f)
            ++negative_samples;
    }
    CAPTURE(negative_samples);
    CHECK(negative_samples > 0);

    // And the control: the curve the rest of the engine uses never undershoots,
    // which is why `<=` looked harmless to everyone who only tried this one.
    for (int i = 0; i <= 64; ++i) {
        const float d = kRadius * static_cast<float>(i) / 64.0f;
        CHECK(cregion_weight(kCentre + cf3(d, 0.0f, 0.0f), kCentre, kRadius, ease_smoothstep) >=
              0.0f);
    }
}

TEST_CASE("cgrab_point early-out is `== 0.0f`: it must not delete the back/elastic undershoot") {
    // The performance early-out has to be EXACTLY free. Anything it changes is
    // a behaviour change smuggled in as an optimisation, so it is compared to
    // the no-early-out definition to the BIT rather than to a tolerance — an
    // Approx here would accept a `<=` build at every probe whose undershoot is
    // small, which is most of them.
    for (int ease = 0; ease < ease_count; ++ease) {
        CAPTURE(ease);
        for (int front = 0; front < 2; ++front) {
            CAPTURE(front);
            const float fo = static_cast<float>(front);
            for (int i = 0; i < 24; ++i) {
                // Out to 1.25r, so the samples span the inside, the rim and the
                // outside. `radius * i / 16` keeps the step a clean binary
                // fraction.
                const float d = kRadius * static_cast<float>(i) / 16.0f;
                const cfloat3 p = kCentre + cf3(d * 0.75f, d * 0.5f, -d * 0.25f);
                CAPTURE(d);
                CHECK(bit_equal(cgrab_point(p, kCentre, kRadius, kDrag, fo, ease),
                                grab_reference(p, kCentre, kRadius, kDrag, fo, ease)));
            }
        }
    }

    // Said directly for the two easings in this file's title, at the probe the
    // sign change was chosen around: a negative weight makes grab sample on the
    // FAR side of the drag, so the material moves the opposite way. A `<=`
    // build returns the probe unchanged and fails here.
    for (int ease : {ease_in_back, ease_in_out_back, ease_in_elastic, ease_in_out_elastic}) {
        CAPTURE(ease);
        const float w = cregion_weight(kInside, kCentre, kRadius, ease);
        if (w >= 0.0f) continue;  // this curve's negative lobe is not at this probe
        const cfloat3 got = cgrab_point(kInside, kCentre, kRadius, kDrag, 0.0f, ease);
        CHECK_FALSE(bit_equal(got, kInside));
        // p - d*w with w < 0 is p + d*|w|: along the drag, not against it.
        CHECK(cdot(got - kInside, kDrag) > 0.0f);
    }
}

TEST_CASE("cmagnify_point guard is `== 0.0f`: `<=` made it inert over 94.9% of the ball") {
    const float strength = 0.5f;
    const float w = cregion_weight(kInside, kCentre, kRadius, ease_in_back);
    REQUIRE(w < 0.0f);

    const cfloat3 got = cmagnify_point(kInside, kCentre, kRadius, strength, ease_in_back);
    // THE REGRESSION. With `w <= 0.0f` this returned `kInside` untouched.
    CHECK_FALSE(bit_equal(got, kInside));

    // And it moved the right way. `scale = max(1 - strength*w, 0.05)` with
    // strength > 0 and w < 0 is greater than one, so the sample is taken FARTHER
    // from the centre than the probe — which is the pinch the back easing's
    // undershoot is asking for, the inverse of the magnification the same
    // deformer gives at a positive weight.
    CHECK(distance(got, kCentre) > distance(kInside, kCentre));
    const float expected_scale = 1.0f - strength * w;
    CHECK(expected_scale > 1.0f);
    CHECK(distance(got, kCentre) ==
          doctest::Approx(distance(kInside, kCentre) * expected_scale).epsilon(1e-6));
    // Big enough to be a visible defect rather than a rounding argument: at
    // these parameters `<=` lost 0.030 world units of sample offset.
    CHECK(distance(got, kInside) > 0.02f);

    // The same at a POSITIVE weight is unaffected by the operator, and is
    // checked so the case above cannot pass by the deformer having become
    // unconditional.
    const cfloat3 near_centre = kCentre + cf3(0.125f, 0.0f, 0.0f);
    REQUIRE(cregion_weight(near_centre, kCentre, kRadius, ease_in_back) > 0.0f);
    CHECK(distance(cmagnify_point(near_centre, kCentre, kRadius, strength, ease_in_back), kCentre) <
          distance(near_centre, kCentre));

    // Outside the region the weight is exactly zero, both operators early out,
    // and finite support still holds — the property the guard is there for in
    // the first place, and the one a fix must not trade away.
    for (int ease : {ease_in_back, ease_in_elastic}) {
        CAPTURE(ease);
        const cfloat3 outside = kCentre + cf3(1.25f, 0.0f, 0.0f);
        REQUIRE(cregion_weight(outside, kCentre, kRadius, ease) == 0.0f);
        CHECK(bit_equal(cmagnify_point(outside, kCentre, kRadius, strength, ease), outside));
    }
}

TEST_CASE("cblob_offset guard is `== 0.0f`: `<=` returned 0 where the amplitude should be negative") {
    const float amplitude = 0.5f;
    const float frequency = 3.0f;
    const int octaves = 3;
    const float gain = 0.5f;
    const cuint seed = 5u;

    for (int ease : {ease_in_back, ease_in_elastic}) {
        CAPTURE(ease);
        // Sweep the region for a probe where the weight is negative AND the
        // fractal is not near a zero crossing of its own — a blob probe needs
        // both, and hard-coding one would pin `cnoise_fbm`'s phase rather than
        // this guard.
        bool found = false;
        for (int i = 1; i < 64 && !found; ++i) {
            const float d = kRadius * static_cast<float>(i) / 64.0f;
            const cfloat3 p = kCentre + cf3(d * 0.75f, d * 0.5f, -d * 0.25f);
            const float w = cregion_weight(p, kCentre, kRadius, ease);
            const float n = cnoise_fbm(p * frequency, octaves, gain, seed);
            if (w >= 0.0f || std::fabs(n) < 0.1f) continue;
            found = true;
            CAPTURE(d);
            const float got =
                cblob_offset(p, kCentre, kRadius, amplitude, frequency, octaves, gain, seed, ease);
            // THE REGRESSION. With `w <= 0.0f` this returned exactly 0.
            CHECK(got != 0.0f);
            CHECK(got == doctest::Approx(amplitude * w * n).epsilon(1e-6));
            // The sign flip is the point, not an artefact: a negative weight
            // turns the swelling into a hollow, so the offset opposes the one
            // the same noise sample would give at a positive weight.
            CHECK(got * (amplitude * n) < 0.0f);
        }
        CHECK(found);
    }

    // Outside the region: exactly zero offset, under both operators, which is
    // what makes blob a brush rather than a whole-item modifier.
    const cfloat3 outside = kCentre + cf3(1.25f, 0.0f, 0.0f);
    for (int ease : {ease_in_back, ease_in_elastic}) {
        CAPTURE(ease);
        REQUIRE(cregion_weight(outside, kCentre, kRadius, ease) == 0.0f);
        CHECK(cblob_offset(outside, kCentre, kRadius, amplitude, frequency, octaves, gain, seed,
                           ease) == 0.0f);
    }
}

TEST_CASE("the zero-weight early-outs fire where the weight is actually zero") {
    // The other half of "exactly free": an early-out that never fires is also
    // bit-identical to no early-out, and would pass every check above while
    // buying nothing. So the samples it is supposed to skip are counted.
    //
    // MEASURED, and this is the number the grab speedup comes from: over a cube
    // of half-extent 1.25r about the centre, 73.6% of samples lie outside the
    // region and carry a weight of exactly zero under ease_smoothstep. That is
    // the fraction of the front gate — a normalize, a dot and a clamp — and of
    // the displacement madd that the early-out removes on a whole-document tape,
    // where nothing has culled the deformer first.
    //
    // The bound is asserted loosely, at "more than half", ON PURPOSE. Pinning
    // 73.6% would make this case fail if the probe cube were ever resized, which
    // would say nothing about the kernel. What it has to rule out is the
    // early-out being dead code.
    int zero = 0, total = 0;
    const int n = 24;
    for (int ix = 0; ix < n; ++ix)
        for (int iy = 0; iy < n; ++iy)
            for (int iz = 0; iz < n; ++iz) {
                const cfloat3 p =
                    kCentre + cf3(-1.25f + 2.5f * (static_cast<float>(ix) + 0.5f) / n,
                                  -1.25f + 2.5f * (static_cast<float>(iy) + 0.5f) / n,
                                  -1.25f + 2.5f * (static_cast<float>(iz) + 0.5f) / n);
                ++total;
                if (cregion_weight(p, kCentre, kRadius, ease_smoothstep) == 0.0f) ++zero;
            }
    CAPTURE(zero);
    CAPTURE(total);
    CHECK(zero > total / 2);
}
