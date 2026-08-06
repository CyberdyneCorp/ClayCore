// The loft opcode (sdf-kernels + scene-model specs, add-loft-opcode).

#include <doctest/doctest.h>

#include "clay/io/clayspace.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf2;
using kernel::cf3;
using scene::Prim;
using scene::Profile;

namespace {

scene::Node loft_node(std::vector<Profile> profiles,
                      std::vector<std::vector<kernel::cfloat2>> polygons, float half_depth = 1.0f,
                      std::uint8_t ease = 0) {
    scene::Node n;
    n.prim = Prim::loft(half_depth, ease);
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

// A circle at the bottom, a much smaller circle at the top: the cross-section
// at each end is unambiguous and the middle is unambiguously between them.
scene::Node cone_ish(float half_depth = 1.0f, std::uint8_t ease = 0) {
    return loft_node({Profile::circle(1.0f), Profile::circle(0.25f)}, {{}, {}}, half_depth, ease);
}

}  // namespace

TEST_CASE("loft: each end matches its own profile") {
    scene::Tape tape = compile_one(cone_ish());

    // At z = -h the cross-section is the bottom circle: a point just inside
    // radius 1 is material, one outside is not.
    CHECK(tape.eval(cf3(0.9f, 0, -0.99f)).d < 0.0f);
    CHECK(tape.eval(cf3(1.1f, 0, -0.99f)).d > 0.0f);
    // At z = +h it is the top circle, radius 0.25.
    CHECK(tape.eval(cf3(0.2f, 0, 0.99f)).d < 0.0f);
    CHECK(tape.eval(cf3(0.4f, 0, 0.99f)).d > 0.0f);
    // ...and outside the depth there is nothing at all.
    CHECK(tape.eval(cf3(0, 0, 1.5f)).d > 0.0f);
    CHECK(tape.eval(cf3(0, 0, -1.5f)).d > 0.0f);
}

TEST_CASE("loft: the middle lies between the two profiles") {
    scene::Tape tape = compile_one(cone_ish());
    // Halfway the radius is about (1 + 0.25) / 2 = 0.625: inside the bottom
    // profile's radius and outside the top's, so it matches neither.
    CHECK(tape.eval(cf3(0.55f, 0, 0.0f)).d < 0.0f);
    CHECK(tape.eval(cf3(0.7f, 0, 0.0f)).d > 0.0f);
}

TEST_CASE("loft: more than two profiles are bracketed") {
    // Wide, narrow, wide: the waist is only visible if the middle profile is
    // actually reached rather than interpolated past.
    scene::Node n = loft_node({Profile::circle(1.0f), Profile::circle(0.2f), Profile::circle(1.0f)},
                              {{}, {}, {}}, 1.0f);
    scene::Tape tape = compile_one(n);

    CHECK(tape.eval(cf3(0.9f, 0, -0.99f)).d < 0.0f);   // wide at the bottom
    CHECK(tape.eval(cf3(0.9f, 0, 0.99f)).d < 0.0f);    // wide at the top
    CHECK(tape.eval(cf3(0.5f, 0, 0.0f)).d > 0.0f);     // pinched in the middle
    CHECK(tape.eval(cf3(0.15f, 0, 0.0f)).d < 0.0f);

    SUBCASE("two profiles would not pinch") {
        scene::Tape straight =
            compile_one(loft_node({Profile::circle(1.0f), Profile::circle(1.0f)}, {{}, {}}));
        CHECK(straight.eval(cf3(0.5f, 0, 0.0f)).d < 0.0f);
    }
}

TEST_CASE("loft: a polygon profile carries its own vertices") {
    // A circle to a square, so both the parametric and the out-of-line profile
    // paths are exercised in one item — and their blob offsets must not
    // collide, which is the thing most likely to be wrong.
    scene::Node n = loft_node(
        {Profile::circle(0.9f), Profile::polygon()},
        {{}, {cf2(-0.5f, -0.5f), cf2(0.5f, -0.5f), cf2(0.5f, 0.5f), cf2(-0.5f, 0.5f)}});
    scene::Tape tape = compile_one(n);

    CHECK(tape.eval(cf3(0.85f, 0, -0.99f)).d < 0.0f);   // circle at the bottom
    CHECK(tape.eval(cf3(0.45f, 0.45f, 0.99f)).d < 0.0f);  // the square's corner at the top
    CHECK(tape.eval(cf3(0.7f, 0.7f, 0.99f)).d > 0.0f);    // outside it

    SUBCASE("two polygon profiles keep their own vertices") {
        scene::Node two = loft_node(
            {Profile::polygon(), Profile::polygon()},
            {{cf2(-1.0f, -1.0f), cf2(1.0f, -1.0f), cf2(1.0f, 1.0f), cf2(-1.0f, 1.0f)},
             {cf2(-0.2f, -0.2f), cf2(0.2f, -0.2f), cf2(0.2f, 0.2f), cf2(-0.2f, 0.2f)}});
        scene::Tape t = compile_one(two);
        CHECK(t.eval(cf3(0.9f, 0.9f, -0.99f)).d < 0.0f);   // the big square
        CHECK(t.eval(cf3(0.9f, 0.9f, 0.99f)).d > 0.0f);    // not the small one
        CHECK(t.eval(cf3(0.15f, 0.15f, 0.99f)).d < 0.0f);
    }
}

TEST_CASE("loft: the ease shapes the interpolation") {
    scene::Tape linear = compile_one(cone_ish(1.0f, 0));
    scene::Tape eased = compile_one(cone_ish(1.0f, kernel::ease_smoothstep));
    // A quarter of the way up, smoothstep has moved less than linear has, so
    // the cross-section is still wider.
    float z = -0.5f;
    bool differs = false;
    for (float r = 0.3f; r < 1.0f; r += 0.05f)
        if ((linear.eval(cf3(r, 0, z)).d < 0.0f) != (eased.eval(cf3(r, 0, z)).d < 0.0f))
            differs = true;
    CHECK(differs);
}

// The requirement the raymarcher depends on: a loft is a bound, and its
// Lipschitz is not one.
TEST_CASE("loft: exactness and the safe step scale are declared") {
    scene::Tape tape = compile_one(cone_ish());
    CHECK_FALSE(tape.info.is_exact);
    CHECK(tape.info.lipschitz > 1.0f);
    CHECK(tape.safe_step_scale() < 1.0f);

    SUBCASE("differing profiles over a shallow depth step more carefully") {
        // Same profiles, a tenth of the depth: the field changes ten times as
        // fast along Z, so the safe step must fall.
        scene::Tape shallow = compile_one(cone_ish(0.1f));
        CHECK(shallow.safe_step_scale() < tape.safe_step_scale());
    }

    SUBCASE("identical profiles are still a bound, but a gentle one") {
        scene::Tape flat =
            compile_one(loft_node({Profile::circle(0.5f), Profile::circle(0.5f)}, {{}, {}}));
        CHECK_FALSE(flat.info.is_exact);
        CHECK(flat.safe_step_scale() > tape.safe_step_scale());
    }
}

TEST_CASE("loft: bounds cover every profile") {
    scene::Node n = loft_node({Profile::circle(0.2f), Profile::circle(1.5f)}, {{}, {}}, 0.7f);
    math::Aabb b = scene::item_local_bounds(n);
    CHECK(b.max.x >= 1.5f);   // the widest profile, not the first
    CHECK(b.max.y >= 1.5f);
    CHECK(b.max.z == doctest::Approx(0.7f));
    CHECK(b.min.z == doctest::Approx(-0.7f));
}

TEST_CASE("loft: round trips through the document format") {
    io::ClaySpaceDoc file;
    scene::Layer& layer = file.document.add_sdf_layer("l");
    layer.sdf->insert(loft_node(
        {Profile::circle(0.9f), Profile::polygon(), Profile::box(0.3f, 0.6f)},
        {{},
         {cf2(-0.5f, -0.5f), cf2(0.5f, -0.5f), cf2(0.0f, 0.6f)},
         {}},
        1.3f, kernel::ease_smoothstep));

    std::vector<std::uint8_t> bytes = io::save_clayspace(file);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    const scene::Layer* l = back.document.find_layer(layer.id);
    REQUIRE(l);
    const scene::Node* n = l->sdf->find(l->sdf->roots[0]);
    REQUIRE(n);
    REQUIRE(n->profiles.size() == 3);
    CHECK(n->profiles[0].type == kernel::cprofile_circle);
    CHECK(n->profiles[0].params[0] == doctest::Approx(0.9f));
    CHECK(n->profiles[1].is_polygon());
    REQUIRE(n->profile_polygons.size() == 3);
    CHECK(n->profile_polygons[1].size() == 3);
    CHECK(n->profile_polygons[1][2].y == doctest::Approx(0.6f));
    CHECK(n->profiles[2].params[1] == doctest::Approx(0.6f));
    // ...and the field is what it was.
    CHECK(scene::compile_document(back.document).blob ==
          scene::compile_document(file.document).blob);
}

// The change must not move any existing document.
TEST_CASE("loft: existing lifts compile exactly as before") {
    scene::Node extrude;
    extrude.prim = Prim::extrude(0.5f);
    extrude.profile = Profile::hexagon(0.8f);
    scene::Tape a = compile_one(extrude);

    scene::Node revolve;
    revolve.prim = Prim::revolve(1.2f);
    revolve.profile = Profile::polygon();
    revolve.profile_points = {cf2(-0.3f, -0.3f), cf2(0.3f, -0.3f), cf2(0.3f, 0.3f)};
    scene::Tape b = compile_one(revolve);

    // Both are still exact, still use the single-profile field, and carry no
    // profile list — the loft's storage is separate precisely so this holds.
    CHECK(a.info.is_exact);
    CHECK(b.info.is_exact);
    CHECK(a.blob.empty());
    CHECK(b.blob.size() == 6);  // three vertices, x and y each
}
