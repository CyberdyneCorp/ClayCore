// Alphas on SDF layers (sdf-kernels spec, add-sdf-alphas).
//
// A caller-supplied scalar stamp as a distance offset under finite support —
// the technique every competing sculptor details with, and the one this engine
// had on voxels and not on fields.
//
// The claims that matter are the ones a raymarcher depends on: an untouched
// stamp is EXACTLY the identity, material outside the radius is EXACTLY
// untouched, and the Lipschitz bound comes from the stamp's STEEPNESS rather
// than its values — so a flat stamp of ones costs nothing for being large.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/brush/alpha_stamp.h"
#include "clay/kernel/deform.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::kernel;
using scene::Deformer;

namespace {

constexpr float kRadius = 0.5f;  // the sphere
constexpr float kCentre = 0.5f;  // the stamp sits on its +Z pole
constexpr float kRegion = 0.4f;
constexpr float kExtent = 0.6f;

scene::Tape stamped(const std::vector<float>& samples, int w, int h, float amplitude,
                    float extent = kExtent, cfloat3 tangent = cf3(1, 0, 0)) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::sphere(kRadius);
    if (!samples.empty())
        n.deformers.push_back(Deformer::alpha(cf3(0, 0, kCentre), cf3(0, 0, 1), tangent,
                                              samples.data(), w, h, extent, kRegion, amplitude));
    l.sdf->insert(n);
    return scene::compile_document(doc);
}

// Where the surface crosses +Z, by bisection on the field.
float surface_z(const scene::Tape& t) {
    float lo = 0.0f, hi = 3.0f;
    for (int i = 0; i < 80; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (t.eval(cf3(0, 0, mid)).d < 0.0f)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5f * (lo + hi);
}

std::vector<float> filled(int w, int h, float v) {
    return std::vector<float>(static_cast<std::size_t>(w) * h, v);
}

// The steepest stamp there is at a given size: adjacent samples alternate.
std::vector<float> checker(int w, int h) {
    std::vector<float> s(static_cast<std::size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            s[static_cast<std::size_t>(y) * w + x] = ((x + y) & 1) ? 1.0f : 0.0f;
    return s;
}

}  // namespace

TEST_CASE("an all-zero stamp is exactly the item without it") {
    // Exactly, not nearly: the offset is a product with the sample, so a zero
    // sample contributes a hard zero and there is no epsilon to argue about.
    const scene::Tape plain = stamped({}, 0, 0, 0.0f);
    const scene::Tape zeroed = stamped(filled(16, 16, 0.0f), 16, 16, 0.5f);

    for (int i = 0; i < 512; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 9 - 4) * 0.22f, static_cast<float>((i / 9) % 9 - 4) * 0.22f,
                static_cast<float>((i / 81) % 9 - 4) * 0.22f);
        REQUIRE(plain.eval(p).d == zeroed.eval(p).d);
    }
    // ...and it costs no step scale either, so an unused stamp is free.
    CHECK(zeroed.safe_step_scale() == doctest::Approx(plain.safe_step_scale()));
}

TEST_CASE("a zero amplitude is exactly the identity whatever the stamp says") {
    // The other way to say "off": a caller dialling amplitude to zero must get
    // the item back exactly, not approximately, or a strength slider cannot
    // reach the original.
    const scene::Tape plain = stamped({}, 0, 0, 0.0f);
    const scene::Tape off = stamped(checker(16, 16), 16, 16, 0.0f);
    for (int i = 0; i < 256; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 7 - 3) * 0.25f, static_cast<float>((i / 7) % 7 - 3) * 0.25f,
                static_cast<float>((i / 49) % 7 - 3) * 0.25f);
        REQUIRE(plain.eval(p).d == off.eval(p).d);
    }
}

TEST_CASE("white is raised and black is carved") {
    // The sign convention, which is a footgun if it is wrong: every sculpting
    // package treats white in an alpha as raised. A deformer's offset is ADDED
    // to the distance, where positive means further away, so the kernel flips
    // it once — and this is the test that says it flipped.
    const scene::Tape plain = stamped({}, 0, 0, 0.0f);
    const float bare = surface_z(plain);
    CHECK(bare == doctest::Approx(kRadius));

    const scene::Tape raised = stamped(filled(16, 16, 1.0f), 16, 16, 0.15f);
    const scene::Tape carved = stamped(filled(16, 16, 1.0f), 16, 16, -0.15f);
    CHECK(surface_z(raised) > bare);
    CHECK(surface_z(carved) < bare);
    // Symmetric about the original, since the weight at the pole is the same.
    CHECK(surface_z(raised) - bare == doctest::Approx(bare - surface_z(carved)).epsilon(0.02));
}

TEST_CASE("material outside the region is untouched, exactly") {
    // What makes this a brush rather than a modifier, and what keeps the
    // influence bound tight enough for culling to be worth anything.
    const scene::Tape plain = stamped({}, 0, 0, 0.0f);
    const scene::Tape marked = stamped(checker(16, 16), 16, 16, 0.2f);

    // The far pole and the equator are both well outside the region, whose
    // centre is the +Z pole and whose radius is kRegion.
    for (const cfloat3 p : {cf3(0, 0, -0.9f), cf3(0, 0, -kRadius), cf3(0.9f, 0, 0),
                            cf3(0, -0.9f, 0), cf3(-0.7f, -0.7f, -0.2f)}) {
        CAPTURE(p.x);
        CAPTURE(p.y);
        CAPTURE(p.z);
        REQUIRE(clength(p - cf3(0, 0, kCentre)) > kRegion);  // the fixture is honest
        CHECK(plain.eval(p).d == marked.eval(p).d);
    }
}

TEST_CASE("the Lipschitz bound comes from steepness, not from sample values") {
    // The heart of the change. A stamp of all ones has the largest possible
    // VALUES and is perfectly flat, so it displaces rigidly and adds no
    // steepness. A bound taken from magnitudes would charge it as though it
    // were the steepest stamp there is.
    const float amp = 0.15f;
    const scene::Tape flat = stamped(filled(16, 16, 1.0f), 16, 16, amp);
    const scene::Tape steep = stamped(checker(16, 16), 16, 16, amp);
    CAPTURE(flat.safe_step_scale());
    CAPTURE(steep.safe_step_scale());
    CHECK(steep.safe_step_scale() < flat.safe_step_scale());

    // A flat stamp's cost does not change with RESOLUTION: it is the same
    // rigid displacement whether it is 4x4 or 256x256. A magnitude-based bound
    // would say the same thing here, which is why the case above matters — but
    // a bound over raw differences without the texel spacing would not.
    for (int n : {4, 32, 256}) {
        CAPTURE(n);
        CHECK(stamped(filled(n, n, 1.0f), n, n, amp).safe_step_scale() ==
              doctest::Approx(flat.safe_step_scale()));
    }

    // ...and a steeper stamp at the SAME peak costs more, which is the
    // property that separates the two bounds.
    CHECK(stamped(checker(64, 64), 64, 64, amp).safe_step_scale() <
          stamped(checker(8, 8), 8, 8, amp).safe_step_scale());
}

TEST_CASE("the same relief spread wider is less steep") {
    // The bound is a WORLD slope, so the same samples over a larger extent are
    // a gentler surface and cost less. A bound over raw sample differences
    // would miss this entirely and charge both the same.
    const std::vector<float> s = checker(32, 32);
    const scene::Tape tight = stamped(s, 32, 32, 0.15f, 0.2f);
    const scene::Tape wide = stamped(s, 32, 32, 0.15f, 1.2f);
    CAPTURE(tight.safe_step_scale());
    CAPTURE(wide.safe_step_scale());
    CHECK(wide.safe_step_scale() > tight.safe_step_scale());
}

TEST_CASE("the declared step scale does not overshoot the surface") {
    // The bound is only worth having if marching by it is safe. Walk the ray
    // the tape says is safe and check no step ever crosses the surface — which
    // is what an under-bound would show up as, and what a render would show as
    // holes.
    const scene::Tape t = stamped(checker(32, 32), 32, 32, 0.2f);
    const float scale = t.safe_step_scale();
    REQUIRE(scale > 0.0f);

    // The invariant sphere tracing actually rests on: the reported distance is
    // an underestimate of the true one by at most the factor `scale` corrects
    // for, so from a point where the field reads d > 0, advancing scale*d
    // cannot land INSIDE. If it ever does, the bound is understated and a
    // renderer marching by it punches holes through the relief.
    //
    // Stated as f(x + scale*d) >= 0 rather than as a bound on how fast f may
    // fall: the field's Lipschitz constant is 1/scale, not 1, so "the value
    // cannot drop by more than the distance travelled" is a claim about a
    // different field than this one.
    for (int i = 0; i < 24; ++i) {
        const float a = static_cast<float>(i) * 0.26f;
        const cfloat3 origin = cf3(std::sin(a) * 0.35f, std::cos(a) * 0.35f, 2.0f);
        const cfloat3 dir = cf3(0, 0, -1);
        float travelled = 0.0f;
        for (int step = 0; step < 2000 && travelled < 3.0f; ++step) {
            const float d = t.eval(origin + dir * travelled).d;
            if (d <= 1e-5f) break;  // arrived, which is the point
            const float advance = scale * d;
            const float next = t.eval(origin + dir * (travelled + advance)).d;
            CAPTURE(i);
            CAPTURE(step);
            CAPTURE(travelled);
            CAPTURE(d);
            CAPTURE(next);
            REQUIRE(next >= -1e-4f);  // float slack only
            travelled += advance;
        }
    }
}

TEST_CASE("a stamp survives serialization, bound and all") {
    std::vector<float> s = checker(16, 16);
    s[0] = 0.25f;  // something asymmetric, so a transposed read shows up
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::sphere(kRadius);
    n.deformers.push_back(Deformer::alpha(cf3(0, 0, kCentre), cf3(0, 0, 1), cf3(1, 0, 0), s.data(),
                                          16, 16, kExtent, kRegion, 0.2f));
    l.sdf->insert(n);

    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(scene::serialize_document(*back) == bytes);  // canonical

    // The field is identical, which is the claim that matters...
    const scene::Tape a = scene::compile_document(doc);
    const scene::Tape b = scene::compile_document(*back);
    for (int i = 0; i < 512; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 9 - 4) * 0.2f, static_cast<float>((i / 9) % 9 - 4) * 0.2f,
                static_cast<float>((i / 81) % 9 - 4) * 0.2f);
        REQUIRE(a.eval(p).d == b.eval(p).d);
    }
    // ...and so is the bound, which is recomputed on load rather than read, so
    // a hand-edited file cannot claim a looser one than its samples deserve.
    CHECK(a.safe_step_scale() == doctest::Approx(b.safe_step_scale()));
}

TEST_CASE("a malformed stamp is inert rather than dangerous") {
    // A stamp too small to interpolate, or one whose samples do not match its
    // dimensions, must not read past its buffer or produce a bound it cannot
    // honour. Inert is the right answer: the caller can still fix it.
    const scene::Tape plain = stamped({}, 0, 0, 0.0f);
    const float bare = surface_z(plain);

    // A 1x1 stamp: no adjacent samples, so nothing to interpolate.
    CHECK(surface_z(stamped(filled(1, 1, 1.0f), 1, 1, 0.3f)) == doctest::Approx(bare));
    // A degenerate extent.
    CHECK(surface_z(stamped(filled(16, 16, 1.0f), 16, 16, 0.3f, 0.0f)) == doctest::Approx(bare));

    // Dimensions that disagree with the sample count: the factory refuses to
    // copy, leaving an empty stamp rather than one that would read past the end.
    const std::vector<float> few = filled(4, 4, 1.0f);
    Deformer d = Deformer::alpha(cf3(0, 0, kCentre), cf3(0, 0, 1), cf3(1, 0, 0), few.data(), 4, 4,
                                 kExtent, kRegion, 0.3f);
    CHECK(!d.stamp.empty());
    Deformer bad = Deformer::alpha(cf3(0, 0, kCentre), cf3(0, 0, 1), cf3(1, 0, 0), nullptr, 16, 16,
                                   kExtent, kRegion, 0.3f);
    CHECK(bad.stamp.empty());
    CHECK(bad.stamp.peak == 0.0f);
    CHECK(bad.stamp.world_slope() == 0.0f);
}

TEST_CASE("a degenerate tangent still gives a usable frame") {
    // A caller passing an "up" parallel to the push direction is a caller, not
    // an error: the frame falls back to a derived axis rather than collapsing,
    // which would put a divide by zero in the middle of every sample.
    const std::vector<float> s = checker(16, 16);
    const scene::Tape parallel = stamped(s, 16, 16, 0.15f, kExtent, cf3(0, 0, 1));
    const scene::Tape zero = stamped(s, 16, 16, 0.15f, kExtent, cf3(0, 0, 0));
    for (const scene::Tape* t : {&parallel, &zero}) {
        CHECK(std::isfinite(t->safe_step_scale()));
        CHECK(t->safe_step_scale() > 0.0f);
        CHECK(std::isfinite(t->eval(cf3(0, 0, 0.5f)).d));
    }

    // ...and a rough tangent is re-orthogonalised rather than shearing the
    // stamp: an up that leans out of the plane gives the same field as its
    // projection into it.
    const scene::Tape rough = stamped(s, 16, 16, 0.15f, kExtent, cf3(1, 0, 0.7f));
    const scene::Tape clean = stamped(s, 16, 16, 0.15f, kExtent, cf3(1, 0, 0));
    for (int i = 0; i < 128; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 5 - 2) * 0.15f, static_cast<float>((i / 5) % 5 - 2) * 0.15f,
                0.4f + static_cast<float>((i / 25) % 5) * 0.06f);
        REQUIRE(rough.eval(p).d == doctest::Approx(clean.eval(p).d));
    }
}

// -- placement ---------------------------------------------------------------

TEST_CASE("a placement built from a hit faces the surface") {
    const cfloat3 point = cf3(0, 0, kRadius);
    const cfloat3 normal = cf3(0, 0, 1);
    const brush::StampPlacement pl = brush::stamp_placement(point, normal);

    CHECK(pl.centre.z == doctest::Approx(kRadius));
    CHECK(pl.direction.z == doctest::Approx(1.0f));
    // The tangent lies IN the stamp's plane, which is the property everything
    // downstream assumes.
    CHECK(cdot(pl.tangent, pl.direction) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(clength(pl.tangent) == doctest::Approx(1.0f));
    // An unnormalized normal is normalized rather than refused.
    const brush::StampPlacement scaled = brush::stamp_placement(point, cf3(0, 0, 7.5f));
    CHECK(clength(scaled.direction) == doctest::Approx(1.0f));
}

TEST_CASE("a placement is stable when the up direction is useless") {
    // The workflow property, not the maths one: any perpendicular is a valid
    // tangent, but one that JUMPS as the camera crosses the normal makes a
    // stamp spin under the cursor. The fallback is derived from the normal, so
    // it is the same answer every time for the same surface.
    const cfloat3 n = cf3(0, 1, 0);
    const brush::StampPlacement a = brush::stamp_placement(cf3(0, 1, 0), n, n);
    const brush::StampPlacement b = brush::stamp_placement(cf3(0, 1, 0), n, n * -3.0f);
    const brush::StampPlacement c = brush::stamp_placement(cf3(0, 1, 0), n, cf3(0, 0, 0));
    CHECK(clength(a.tangent) == doctest::Approx(1.0f));
    CHECK(cdot(a.tangent, n) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(a.tangent.x == doctest::Approx(b.tangent.x));
    CHECK(a.tangent.z == doctest::Approx(b.tangent.z));
    CHECK(a.tangent.x == doctest::Approx(c.tangent.x));
    CHECK(a.tangent.z == doctest::Approx(c.tangent.z));
}

TEST_CASE("roll turns the stamp in its own plane") {
    const cfloat3 n = cf3(0, 0, 1);
    const brush::StampPlacement none = brush::stamp_placement(cf3(0, 0, 1), n, cf3(0, 1, 0), 0.0f);
    const brush::StampPlacement quarter =
        brush::stamp_placement(cf3(0, 0, 1), n, cf3(0, 1, 0), 1.5707963f);

    // Still in the plane, still unit, and a quarter turn from where it was.
    CHECK(cdot(quarter.tangent, n) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(clength(quarter.tangent) == doctest::Approx(1.0f));
    CHECK(cdot(quarter.tangent, none.tangent) == doctest::Approx(0.0f).epsilon(1e-5));

    // A full turn comes back.
    const brush::StampPlacement full =
        brush::stamp_placement(cf3(0, 0, 1), n, cf3(0, 1, 0), 6.2831853f);
    CHECK(cdot(full.tangent, none.tangent) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("the placement helper and the deformer agree on the frame") {
    // The whole reason the helper exists: a host that derives its own tangent
    // and one that asks for the placement must get the SAME field, or a
    // preview and its commit differ by a rotation.
    const std::vector<float> s = checker(16, 16);
    const cfloat3 point = cf3(0, 0, kCentre);
    const brush::StampPlacement pl = brush::stamp_placement(point, cf3(0, 0, 1), cf3(0, 1, 0));

    scene::Document viaHelper;
    scene::Node a;
    a.prim = scene::Prim::sphere(kRadius);
    a.deformers.push_back(brush::stamp_deformer(pl, s.data(), 16, 16, kExtent, kRegion, 0.15f));
    viaHelper.add_sdf_layer("l").sdf->insert(a);

    scene::Document viaHand;
    scene::Node b;
    b.prim = scene::Prim::sphere(kRadius);
    b.deformers.push_back(Deformer::alpha(pl.centre, pl.direction, pl.tangent, s.data(), 16, 16,
                                          kExtent, kRegion, 0.15f));
    viaHand.add_sdf_layer("l").sdf->insert(b);

    const scene::Tape x = scene::compile_document(viaHelper);
    const scene::Tape y = scene::compile_document(viaHand);
    for (int i = 0; i < 256; ++i) {
        const cfloat3 p =
            cf3(static_cast<float>(i % 7 - 3) * 0.15f, static_cast<float>((i / 7) % 7 - 3) * 0.15f,
                0.35f + static_cast<float>((i / 49) % 5) * 0.08f);
        REQUIRE(x.eval(p).d == y.eval(p).d);
    }
}

// -- the shim's cast macros --------------------------------------------------

TEST_CASE("the dialect's cast macros survive an expression argument") {
    // Regression for the defect that ate a row of an alpha stamp: CLAY_FLOATC
    // was defined as `(float)x` with no parentheses, so CLAY_FLOATC(h - 1)
    // expanded to `((float)h) - 1`. A C-style cast binds tighter than any
    // binary operator, so it compiled, ran, and indexed one row before the
    // buffer — caught by ASan, invisible to a green suite.
    //
    // Every call site that passed an expression had been working around it
    // with a second pair of parentheses; this checks the macro itself, so the
    // workaround is not load-bearing.
    //
    // The GLSL branch spells these as functional casts — float(x) — which never
    // had the problem, which is exactly why the dialect gate could not catch
    // it: the broken form only exists on the profiles that compile as C++.
    const int h = 32;
    const int w = 16;
    CHECK(CLAY_FLOATC(h - 1) == 31.0f);
    CHECK(CLAY_FLOATC(w - 1) == 15.0f);
    CHECK(CLAY_FLOATC(h - 1) * 0.5f == 15.5f);
    CHECK(CLAY_INT(2.9f + 0.2f) == 3);
    CHECK(CLAY_UINT(7.0f - 2.0f) == 5u);

    // The shape the alpha kernel actually depends on: a normalized coordinate
    // scaled onto the sample grid never leaves [0, h-1], so floor() of it is a
    // valid row.
    for (float v : {0.0f, 1e-4f, 0.5f, 1.0f - 1e-4f, 1.0f}) {
        const float y = cclamp(v, 0.0f, 1.0f) * CLAY_FLOATC(h - 1);
        CAPTURE(v);
        CAPTURE(y);
        CHECK(y >= 0.0f);
        CHECK(y <= static_cast<float>(h - 1));
        CHECK(static_cast<int>(std::floor(y)) >= 0);
    }
}

TEST_CASE("every corner of a stamp is sampled without leaving it") {
    // The direct guard on the overflow: walk points that map to the extremes of
    // (u, v) — including outside the footprint, where the clamp is what keeps
    // the index in range — and require a finite field everywhere. Under ASan
    // this is the case that fails if the indexing regresses.
    const scene::Tape t = stamped(checker(32, 32), 32, 32, 0.2f);
    for (int i = 0; i <= 40; ++i) {
        for (int j = 0; j <= 40; ++j) {
            // Deliberately overshoots the extent by 50% on both axes.
            const float u = (static_cast<float>(i) / 40.0f - 0.5f) * 1.5f * kExtent;
            const float v = (static_cast<float>(j) / 40.0f - 0.5f) * 1.5f * kExtent;
            const cfloat3 p = cf3(u, v, kCentre);
            REQUIRE(std::isfinite(t.eval(p).d));
        }
    }
}
