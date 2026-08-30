#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// The cull pad as a CACHE KEY.
//
// A brick's stored value is keyed on the pad by exact equality, so a pad that
// changes on every append invalidates every seed on every dab. The raw
// envelope did exactly that between 76 nodes and the support clamp, and the
// brick resume was not degraded there but dead: measured on the reference
// iPad, a dab in the middle of a blockout cost 31x a dab at the start and
// cleared the whole interactive frame share.
//
// Quantising the envelope is what holds the pad still. These pin the three
// things that makes true: it does not move between steps, it is never SMALLER
// than the fit it replaces, and the culled field it produces is still
// band-clamp identical to the full one.

using namespace clay;
using kernel::cf3;

namespace {

// The measured fits, mirrored from envelope_fit_for in src/scene/bounds.cpp.
// Duplicated deliberately and narrowly: this file's subject is the RELATION
// between the quantised value and the fit, so it has to be able to name the
// fit. If the coefficients move, this is meant to be updated with them.
struct Fit {
    scene::BlendProfile profile;
    float base, slope;
};
constexpr Fit kFits[] = {
    {scene::BlendProfile::Quadratic, 2.80f, 0.35f},
    {scene::BlendProfile::Cubic, 2.75f, 0.50f},
    {scene::BlendProfile::Circular, 2.70f, 0.30f},
};

float raw_envelope(const Fit& f, std::size_t n) {
    if (n <= 75) return f.base;
    return f.base + f.slope * std::log2(static_cast<float>(n) / 75.0f);
}

// A smooth-blended document, which is what a clay or build brush makes and
// what every SDF fixture in the device suite was NOT until this was found.
scene::Document smooth_document(int count, float k = 0.05f) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    const double m[3] = {0.4142135624, 0.7320508076, 0.2360679775};
    for (int i = 0; i < count; ++i) {
        float p[3];
        for (int a = 0; a < 3; ++a)
            p[a] = float(std::fmod(double(i) * m[a], 1.0)) * 1.6f - 0.8f;
        scene::Node n = clay_test::item(scene::Prim::sphere(0.12f), cf3(p[0], p[1], p[2]));
        n.blend.profile = scene::BlendProfile::Quadratic;
        n.blend.k = k;
        l.sdf->insert(n);
    }
    return doc;
}

}  // namespace

TEST_CASE("the quantised envelope is never smaller than the fit") {
    // The soundness half, and the direction that matters. A larger pad keeps
    // MORE items in a brick's culled tape and cannot change a band-clamped
    // result; a smaller one drops items the fit says are needed, which is a
    // wrong field rather than a slow one.
    for (const Fit& f : kFits) {
        CAPTURE(static_cast<int>(f.profile));
        for (std::size_t n : {1u, 2u, 74u, 75u, 76u, 100u, 123u, 202u, 331u, 544u, 807u,
                              808u, 893u, 2000u, 20000u}) {
            CAPTURE(n);
            const float got = scene::chain_pad_envelope(f.profile, n);
            CHECK(got >= raw_envelope(f, n));
        }
    }
}

TEST_CASE("the envelope is unchanged at and below the knee") {
    // The step is taken on the GROWTH above the base, so a document that never
    // enters the band resolves exactly what it always did and no existing
    // measurement moves.
    for (const Fit& f : kFits) {
        for (std::size_t n : {1u, 10u, 75u}) {
            CHECK(scene::chain_pad_envelope(f.profile, n) == doctest::Approx(f.base));
        }
    }
}

TEST_CASE("the envelope holds still across an append, except at a step") {
    // The property the whole change exists for. Walking one node at a time
    // across the band, the value may only ever RISE, and it must stay put for
    // long runs rather than moving on every node.
    const scene::BlendProfile profile = scene::BlendProfile::Quadratic;
    std::size_t changes = 0;
    float previous = scene::chain_pad_envelope(profile, 1);
    for (std::size_t n = 2; n <= 2000; ++n) {
        const float now = scene::chain_pad_envelope(profile, n);
        CHECK(now >= previous);  // monotone: a pad may not shrink as a document grows
        if (now != previous) ++changes;
        previous = now;
    }
    // Five steps across the quadratic band, not two thousand. The exact count
    // is the quantum's business; what this pins is the ORDER — a value that
    // moved on every node would report ~1900 here, which is the defect.
    CHECK(changes <= 12);
    CAPTURE(changes);
    MESSAGE("envelope steps between 1 and 2000 nodes: " << changes);
}

TEST_CASE("a stroke crosses at most one step") {
    // The sizing rule for the quantum, stated as a property rather than as
    // arithmetic in a comment: 24 dabs may pay a step once, never twice.
    const scene::BlendProfile profile = scene::BlendProfile::Quadratic;
    for (std::size_t start = 60; start <= 1000; ++start) {
        std::size_t crossings = 0;
        float previous = scene::chain_pad_envelope(profile, start);
        for (std::size_t n = start + 1; n <= start + 24; ++n) {
            const float now = scene::chain_pad_envelope(profile, n);
            if (now != previous) ++crossings;
            previous = now;
        }
        CAPTURE(start);
        CHECK(crossings <= 1);
    }
}

TEST_CASE("the pad a document resolves does not move between steps") {
    // The same property one level up, through cull_pad, which is what the seed
    // is actually keyed on.
    for (int base : {90, 250, 600}) {
        CAPTURE(base);
        scene::Document doc = smooth_document(base);
        scene::Layer& l = *doc.find_layer(doc.layers[0].id);
        const float before = scene::cull_pad(*l.sdf, l);

        // append a dab, as a stroke does
        scene::Node n = clay_test::item(scene::Prim::sphere(0.12f), cf3(0.1f, 0.1f, 0.1f));
        n.blend.profile = scene::BlendProfile::Quadratic;
        n.blend.k = 0.05f;
        l.sdf->insert(n);

        const float after = scene::cull_pad(*l.sdf, l);
        // Bit-identical, not approximately equal: the seed gate is exact
        // equality and an approximate check would pass for a pad that still
        // invalidated every seed.
        CHECK(after == before);
    }
}

TEST_CASE("a culled brick still matches the full tape across the band") {
    // Soundness end to end: the pad exists so a per-brick CULLED tape agrees
    // with the full one on band-clamped values, and a quantised pad that broke
    // that would be a wrong picture rather than a fast one. Run on both sides
    // of every step and inside the band, since a pad that was too small shows
    // up as a drop at one size.
    //
    // DRIFT, bounded and reported, rather than bit-identity — the convention
    // test_cull_index.cpp already uses for this comparison. The envelope is a
    // measured fit carrying a margin, not a proof, and this fixture is denser
    // than the ones it was calibrated on: 40 smooth-blended balls inside a
    // 1.6-unit cube drift by one ulp here BELOW the knee, where this change is
    // a no-op, so bit-identity would be asserting something the engine does
    // not promise and would fail identically before this change.
    const float band = 0.15f;
    for (int count : {40, 90, 130, 300, 700, 1200}) {
        CAPTURE(count);
        scene::Document doc = smooth_document(count);
        const scene::Tape full = scene::compile_document(doc);

        // The caller passes the brick dilated by the BAND; the compiler adds
        // the pad itself (document_pad in tape_build.cpp), which is exactly
        // the quantity under test.
        float worst = 0.0f;
        for (int bx = -2; bx < 2; ++bx)
            for (int by = -2; by < 2; ++by) {
                const kernel::cfloat3 lo = cf3(0.4f * float(bx), 0.4f * float(by), -0.2f);
                const math::Aabb brick{lo, lo + cf3(0.4f, 0.4f, 0.4f)};
                scene::CullRegion cull{brick.dilated(band)};
                const scene::Tape culled = scene::compile_document(doc, &cull);
                for (int i = 0; i <= 4; ++i)
                    for (int j = 0; j <= 4; ++j)
                        for (int k = 0; k <= 4; ++k) {
                            const kernel::cfloat3 p =
                                cf3(brick.min.x + 0.1f * float(i), brick.min.y + 0.1f * float(j),
                                    brick.min.z + 0.1f * float(k));
                            const float a = kernel::cclamp(full.eval(p).d, -band, band);
                            const float b = kernel::cclamp(culled.eval(p).d, -band, band);
                            worst = kernel::cmax(worst, std::fabs(a - b));
                        }
            }
        // A pad that dropped an item a brick needed moves a band-clamped value
        // by a visible fraction of the band, not by an ulp. 1e-5 is four
        // orders of magnitude inside the band and two above the drift seen.
        CHECK(worst < 1.0e-5f);
        MESSAGE("count " << count << ": worst band-clamped drift " << worst);
    }
}
