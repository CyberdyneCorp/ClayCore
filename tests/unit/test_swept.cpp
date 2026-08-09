// Sweeping profiles along a guide (sdf-kernels + scene-model specs,
// add-swept-n).

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/io/clayspace.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf2;
using kernel::cf3;
using scene::Prim;
using scene::Profile;
using scene::StrokePoint;
using scene::StrokePointType;

namespace {

StrokePoint gp(kernel::cfloat3 p, StrokePointType t = StrokePointType::Hard) {
    StrokePoint sp;
    sp.pos = p;
    sp.type = t;
    return sp;
}

scene::Node swept_node(std::vector<StrokePoint> guide, std::vector<Profile> profiles,
                       std::vector<std::vector<kernel::cfloat2>> polygons,
                       std::uint8_t ease = 0, float tolerance = 0.02f) {
    scene::Node n;
    n.prim = Prim::swept(ease);
    n.stroke = std::move(guide);
    n.curve_tolerance = tolerance;
    n.profiles = std::move(profiles);
    n.profile_polygons = std::move(polygons);
    return n;
}

scene::Tape compile_one(const scene::Node& node) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(node);
    return scene::compile_document(doc);
}

// A straight guide along +x, so the sweep should be a capsule.
std::vector<StrokePoint> straight(float half = 1.0f) {
    return {gp(cf3(-half, 0, 0)), gp(cf3(half, 0, 0))};
}

}  // namespace

TEST_CASE("swept: a circle along a straight guide is a cylinder") {
    const float r = 0.3f;
    scene::Tape tape =
        compile_one(swept_node(straight(), {Profile::circle(r), Profile::circle(r)}, {{}, {}}));

    // Where the LATERAL surface is the nearest one, the closed-form capsule is
    // the reference. Nearer the ends than the profile is wide, the flat cap
    // becomes the nearest surface instead and a capsule stops being the right
    // comparison — hence 0.6 rather than 0.95 for a radius of 0.3.
    for (float x = -0.6f; x <= 0.6f; x += 0.11f)
        for (float y = -0.6f; y <= 0.6f; y += 0.13f) {
            kernel::cfloat3 p = cf3(x, y, 0.07f);
            float capsule = kernel::sd_capsule(p, cf3(-1, 0, 0), cf3(1, 0, 0), r);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(tape.eval(p).d == doctest::Approx(capsule).epsilon(0.02));
        }

    // The ENDS are the profile itself — a flat cap, not a rounded one. That is
    // deliberate: a profile need not be a circle, so there is no hemisphere to
    // cap it with. On the axis just past the end the distance is the overshoot.
    CHECK(tape.eval(cf3(-1.4f, 0, 0)).d == doctest::Approx(0.4f).epsilon(0.02));
    CHECK(tape.eval(cf3(1.25f, 0, 0)).d == doctest::Approx(0.25f).epsilon(0.02));
    // ...which a capsule would not give: it would round the corner off.
    CHECK(tape.eval(cf3(-1.4f, 0, 0)).d >
          kernel::sd_capsule(cf3(-1.4f, 0, 0), cf3(-1, 0, 0), cf3(1, 0, 0), r));
    // Just inside the end face, still material.
    CHECK(tape.eval(cf3(-0.99f, 0.2f, 0)).d < 0.0f);
}

TEST_CASE("swept: the sweep follows a bent guide") {
    // An L: along +x, then up +y.
    std::vector<StrokePoint> guide = {gp(cf3(-1, 0, 0)), gp(cf3(0, 0, 0)), gp(cf3(0, 1, 0))};
    scene::Tape tape =
        compile_one(swept_node(guide, {Profile::circle(0.25f), Profile::circle(0.25f)}, {{}, {}}));

    // On both limbs there is material...
    CHECK(tape.eval(cf3(-0.6f, 0, 0)).d < 0.0f);
    CHECK(tape.eval(cf3(0, 0.6f, 0)).d < 0.0f);
    CHECK(tape.eval(cf3(0, 0, 0)).d < 0.0f);
    // ...and off them there is not.
    CHECK(tape.eval(cf3(-0.8f, 0.8f, 0)).d > 0.0f);
    CHECK(tape.eval(cf3(0.8f, 0.8f, 0)).d > 0.0f);
}

TEST_CASE("swept: profiles interpolate along the guide") {
    scene::Tape tape = compile_one(
        swept_node(straight(), {Profile::circle(0.4f), Profile::circle(0.1f)}, {{}, {}}));

    // Wide at the start, narrow at the end.
    CHECK(tape.eval(cf3(-0.95f, 0.3f, 0)).d < 0.0f);
    CHECK(tape.eval(cf3(0.95f, 0.3f, 0)).d > 0.0f);
    CHECK(tape.eval(cf3(0.95f, 0.07f, 0)).d < 0.0f);

    SUBCASE("distributed by arc length, not by vertex index") {
        // The same guide with an extra vertex bunched near the start: if
        // profiles were placed per vertex, that would shift the taper.
        std::vector<StrokePoint> bunched = {gp(cf3(-1, 0, 0)), gp(cf3(-0.9f, 0, 0)),
                                            gp(cf3(1, 0, 0))};
        scene::Tape b = compile_one(
            swept_node(bunched, {Profile::circle(0.4f), Profile::circle(0.1f)}, {{}, {}}));
        for (float x = -0.9f; x <= 0.9f; x += 0.2f)
            CHECK(b.eval(cf3(x, 0.2f, 0)).d ==
                  doctest::Approx(tape.eval(cf3(x, 0.2f, 0)).d).epsilon(0.05));
    }
}

// A Frenet frame would flip here; a transported one does not.
TEST_CASE("swept: the frame does not flip where the guide straightens") {
    // Bend, straighten, bend back — the case that breaks a curvature-derived
    // frame, because the curve's own normal is undefined on the straight part.
    std::vector<StrokePoint> guide = {gp(cf3(-2.0f, 0.0f, 0)), gp(cf3(-1.0f, 0.4f, 0)),
                                      gp(cf3(0.0f, 0.4f, 0)),  gp(cf3(1.0f, 0.4f, 0)),
                                      gp(cf3(2.0f, 0.0f, 0))};
    // A flat profile, so its orientation is visible in the field at all.
    scene::Tape tape = compile_one(
        swept_node(guide, {Profile::box(0.30f, 0.06f), Profile::box(0.30f, 0.06f)}, {{}, {}}));

    // Walk the straight middle and require the cross-section's "thin"
    // direction to stay put: a flipped frame would swap thin for wide and the
    // sign at these probes would change.
    for (float x = -0.8f; x <= 0.8f; x += 0.2f) {
        CAPTURE(x);
        bool thin_in_z = tape.eval(cf3(x, 0.4f, 0.20f)).d > 0.0f;
        bool wide_in_z = tape.eval(cf3(x, 0.4f, 0.02f)).d < 0.0f;
        CHECK(thin_in_z);
        CHECK(wide_in_z);
    }
}

TEST_CASE("swept: exactness and the curvature cost are declared") {
    scene::Tape gentle = compile_one(swept_node(
        {gp(cf3(-2, 0, 0)), gp(cf3(0, 0.3f, 0)), gp(cf3(2, 0, 0))},
        {Profile::circle(0.2f), Profile::circle(0.2f)}, {{}, {}}));
    CHECK_FALSE(gentle.info.is_exact);

    SUBCASE("a sharper guide steps more carefully") {
        scene::Tape sharp = compile_one(swept_node(
            {gp(cf3(-2, 0, 0)), gp(cf3(0, 1.6f, 0)), gp(cf3(2, 0, 0))},
            {Profile::circle(0.2f), Profile::circle(0.2f)}, {{}, {}}));
        CHECK(sharp.safe_step_scale() < gentle.safe_step_scale());
    }

    SUBCASE("a profile wider than the bend degrades rather than failing") {
        // The sweep folds through itself here. It must still compile and
        // evaluate — a guide is editable after the fact — and it must report a
        // tiny step rather than claiming to be a distance field.
        scene::Tape folded = compile_one(swept_node(
            {gp(cf3(-1, 0, 0)), gp(cf3(0, 1.0f, 0)), gp(cf3(1, 0, 0))},
            {Profile::circle(2.0f), Profile::circle(2.0f)}, {{}, {}}));
        CHECK_FALSE(folded.info.is_exact);
        CHECK(folded.safe_step_scale() < 0.01f);
        CHECK(std::isfinite(folded.eval(cf3(0, 0, 0)).d));
    }
}

TEST_CASE("swept: the guide honours its point types") {
    std::vector<kernel::cfloat3> pts = {cf3(-1, 0, 0), cf3(0, 0.8f, 0), cf3(1, 0, 0)};
    std::vector<StrokePoint> hard, spline;
    for (kernel::cfloat3 p : pts) {
        hard.push_back(gp(p, StrokePointType::Hard));
        spline.push_back(gp(p, StrokePointType::Spline));
    }
    scene::Tape a = compile_one(
        swept_node(hard, {Profile::circle(0.15f), Profile::circle(0.15f)}, {{}, {}}));
    scene::Tape b = compile_one(
        swept_node(spline, {Profile::circle(0.15f), Profile::circle(0.15f)}, {{}, {}}));
    CHECK(a.blob.size() < b.blob.size());  // the spline tessellated

    // The spline overshoots the corner, so it reaches where the chain does not.
    bool differs = false;
    for (float x = -0.9f; x <= 0.9f; x += 0.1f)
        for (float y = 0.0f; y <= 1.1f; y += 0.1f)
            if ((a.eval(cf3(x, y, 0)).d < 0.0f) != (b.eval(cf3(x, y, 0)).d < 0.0f))
                differs = true;
    CHECK(differs);
}

TEST_CASE("swept: bounds cover the guide dilated by the widest profile") {
    scene::Node n = swept_node({gp(cf3(-1, 0, 0)), gp(cf3(1, 0, 0))},
                               {Profile::circle(0.2f), Profile::circle(0.6f)}, {{}, {}});
    math::Aabb b = scene::item_local_bounds(n);
    CHECK(b.min.x <= -1.6f);
    CHECK(b.max.x >= 1.6f);
    CHECK(b.max.y >= 0.6f);   // the widest profile, not the first
    CHECK(b.max.z >= 0.6f);
}

TEST_CASE("swept: a degenerate sweep produces nothing rather than reading garbage") {
    // Too few guide points, and too few profiles: the tape would otherwise
    // index a record that was never written.
    scene::Tape no_guide = compile_one(swept_node(
        {gp(cf3(0, 0, 0))}, {Profile::circle(0.2f), Profile::circle(0.2f)}, {{}, {}}));
    CHECK(no_guide.eval(cf3(0, 0, 0)).d > 0.0f);

    scene::Tape no_profiles =
        compile_one(swept_node(straight(), {Profile::circle(0.2f)}, {{}}));
    CHECK(no_profiles.eval(cf3(0, 0, 0)).d > 0.0f);
}

TEST_CASE("swept: round trips through the document format") {
    io::ClaySpaceDoc file;
    scene::Layer& layer = file.document.add_sdf_layer("l");
    std::vector<StrokePoint> guide = {gp(cf3(-1, 0, 0), StrokePointType::Spline),
                                      gp(cf3(0, 0.7f, 0.2f), StrokePointType::Spline),
                                      gp(cf3(1, 0, -0.2f), StrokePointType::Spline)};
    layer.sdf->insert(swept_node(
        guide,
        {Profile::circle(0.3f), Profile::polygon(), Profile::box(0.2f, 0.1f)},
        {{}, {cf2(-0.2f, -0.2f), cf2(0.2f, -0.2f), cf2(0.0f, 0.3f)}, {}}, 3, 0.01f));

    std::vector<std::uint8_t> bytes = io::save_clayspace(file);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    const scene::Layer* l = back.document.find_layer(layer.id);
    REQUIRE(l);
    const scene::Node* n = l->sdf->find(l->sdf->roots[0]);
    REQUIRE(n);
    REQUIRE(n->stroke.size() == 3);
    CHECK(n->stroke[1].type == StrokePointType::Spline);
    CHECK(n->stroke[1].pos.y == doctest::Approx(0.7f));
    REQUIRE(n->profiles.size() == 3);
    CHECK(n->profile_polygons[1].size() == 3);
    CHECK(n->curve_tolerance == doctest::Approx(0.01f));
    CHECK(scene::compile_document(back.document).blob ==
          scene::compile_document(file.document).blob);
}

// Regression (v0.24.0 GPU gates): the OpenCL backend disagreed with the scalar
// reference by 7.7% on the swept parity scene — three of 4096 points, all of
// them outside a bend. The cause was not the OpenCL arithmetic. Any point whose
// nearest guide point is a shared VERTEX is equidistant from the two segments
// meeting there, and those two carry different tangents, so they build
// different frames and resolve the profile at different arc lengths. The winner
// was decided by `d2 < best_d2` with no margin, and the two d2 are not
// reliably identical: the segment that clamps to its FAR end reconstructs that
// vertex as `a + ab * 1`, which need not round to the vertex the next segment
// starts from. One ulp there is worth several percent of the result, and
// backends round it differently.
TEST_CASE("swept: the guide segment pick does not turn on the last ulp") {
    using kernel::cf3;
    // guide vertex layout is pos(3), transported normal(3), arc length(1)
    auto guide_of = [](kernel::cfloat3 v0, kernel::cfloat3 v1, kernel::cfloat3 v2) {
        return std::vector<float>{v0.x, v0.y, v0.z, 0, 0, 1, 0,  //
                                  v1.x, v1.y, v1.z, 0, 0, 1, 1,  //
                                  v2.x, v2.y, v2.z, 0, 0, 1, 2};
    };

    const kernel::cfloat3 v0 = cf3(-1, 0, 0);
    const kernel::cfloat3 v1 = cf3(0, 0, 0);
    // p sits in the outer cone of the corner at v1, so BOTH segments clamp to
    // v1 and are exactly equidistant.
    const kernel::cfloat3 p = cf3(1, -1, 0);

    SUBCASE("an exact tie keeps the earlier segment") {
        // second leg perpendicular to (p - v1): the tie is exact, not merely close
        std::vector<float> guide = guide_of(v0, v1, cf3(0.70710678f, 0.70710678f, 0));
        kernel::CSweepHit hit = kernel::csweep_nearest(guide.data(), 3, p);
        CHECK(hit.seg == 0);
    }

    SUBCASE("a sub-margin lead does not flip the pick") {
        // Tilt the second leg towards p just enough to make its squared distance
        // ~1e-7 smaller in relative terms — the size of the cross-backend
        // rounding noise, and far below the ~7e-3 gap to the next genuinely
        // different segment. The earlier segment must still win, or the same
        // scene evaluates differently on two backends.
        const float lambda = 3.16e-4f;
        kernel::cfloat3 dir = cf3(1.0f + lambda, 1.0f - lambda, 0.0f);
        float inv_len = 1.0f / std::sqrt(kernel::cdot(dir, dir));
        std::vector<float> guide = guide_of(v0, v1, dir * inv_len);

        kernel::CSweepHit hit = kernel::csweep_nearest(guide.data(), 3, p);
        CHECK(hit.seg == 0);

        // ...and the lead really is inside the margin, so the test is pinning
        // the tie-break rather than an ordinary "segment 0 is nearer" case.
        kernel::cfloat3 ab = dir * inv_len;
        float t = kernel::cclamp(kernel::cdot(p - v1, ab) / kernel::cdot(ab, ab), 0.0f, 1.0f);
        kernel::cfloat3 off = p - (v1 + ab * t);
        float d2_second = kernel::cdot(off, off);
        float d2_first = kernel::cdot(p - v1, p - v1);
        CHECK(d2_second < d2_first);
        CHECK((d2_first - d2_second) < CLAY_SWEEP_TIE_REL * d2_first);
    }
}
