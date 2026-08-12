// The cut tool (cut-tool spec, add-cut-tool): a drawn shape becomes an
// ordinary edit item that cuts through the model.

#include <doctest/doctest.h>

#include <cmath>

#include "clay/cut/cut.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using cut::CutFrame;
using cut::CutOptions;
using cut::CutShape;
using kernel::cf2;
using kernel::cf3;

namespace {

// Looking down -Z at a block: the frame a viewport would hand over.
CutFrame front_frame(float z = -4.0f) {
    CutFrame f;
    f.origin = cf3(0, 0, z);
    f.right = cf3(1, 0, 0);
    f.up = cf3(0, 1, 0);
    f.forward = cf3(0, 0, 1);
    return f;
}

// A 2x2x2 block centred on the origin, and the region a caller would pass.
const math::Aabb kRegion{cf3(-1, -1, -1), cf3(1, 1, 1)};

scene::Document block_with(std::optional<scene::Node> cut, scene::Op op) {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node solid;
    solid.prim = scene::Prim::box(cf3(1, 1, 1));
    layer.sdf->insert(std::move(solid));
    if (cut) {
        scene::Node item = *cut;
        item.op = op;
        layer.sdf->insert(std::move(item));
    }
    return doc;
}

float field_at(const scene::Document& doc, kernel::cfloat3 p) {
    return scene::compile_document(doc).eval(p).d;
}

}  // namespace

TEST_CASE("cut: a rectangle cuts a rectangular hole") {
    auto item = cut::cut_item(front_frame(), CutShape::rect(0.4f, 0.4f), kRegion);
    REQUIRE(item.has_value());
    scene::Document doc = block_with(item, scene::Op::Subtract);

    // Inside the drawn rectangle the block is gone, at both faces and between.
    for (float z : {-0.9f, 0.0f, 0.9f}) CHECK(field_at(doc, cf3(0, 0, z)) > 0.0f);
    // Outside it the block is untouched.
    CHECK(field_at(doc, cf3(0.8f, 0.8f, 0.0f)) < 0.0f);
    CHECK(field_at(doc, cf3(-0.8f, 0.0f, 0.0f)) < 0.0f);
    // ...and the boundary really is where it was drawn.
    CHECK(field_at(doc, cf3(0.35f, 0.0f, 0.0f)) > 0.0f);
    CHECK(field_at(doc, cf3(0.45f, 0.0f, 0.0f)) < 0.0f);
}

TEST_CASE("cut: a circle and a polygon cut their own outlines") {
    SUBCASE("circle") {
        auto item = cut::cut_item(front_frame(), CutShape::circle(0.5f), kRegion);
        REQUIRE(item.has_value());
        scene::Document doc = block_with(item, scene::Op::Subtract);
        CHECK(field_at(doc, cf3(0, 0, 0)) > 0.0f);
        // A corner of the circle's bounding square is outside the circle, so
        // it survives — which a rectangle of the same size would not leave.
        CHECK(field_at(doc, cf3(0.45f, 0.45f, 0.0f)) < 0.0f);
    }
    SUBCASE("polygon") {
        // A triangle pointing up: its apex region is cut, its bottom corners
        // are not.
        auto item = cut::cut_item(
            front_frame(),
            CutShape::from_polygon({cf2(-0.6f, -0.4f), cf2(0.6f, -0.4f), cf2(0.0f, 0.6f)}),
            kRegion);
        REQUIRE(item.has_value());
        scene::Document doc = block_with(item, scene::Op::Subtract);
        CHECK(field_at(doc, cf3(0.0f, -0.2f, 0.0f)) > 0.0f);   // inside
        CHECK(field_at(doc, cf3(0.55f, 0.5f, 0.0f)) < 0.0f);   // outside, near the apex
    }
}

// The decision the design rests on: a cut is a prism. If it converged, moving
// the frame along its own sweep would change the solid.
TEST_CASE("cut: the cut is a prism, not a frustum") {
    auto near_frame = cut::cut_item(front_frame(-4.0f), CutShape::rect(0.4f, 0.4f), kRegion);
    auto far_frame = cut::cut_item(front_frame(-40.0f), CutShape::rect(0.4f, 0.4f), kRegion);
    REQUIRE(near_frame.has_value());
    REQUIRE(far_frame.has_value());

    scene::Document a = block_with(near_frame, scene::Op::Subtract);
    scene::Document b = block_with(far_frame, scene::Op::Subtract);
    for (float x = -1.2f; x <= 1.2f; x += 0.17f)
        for (float z = -1.2f; z <= 1.2f; z += 0.19f) {
            kernel::cfloat3 p = cf3(x, 0.11f, z);
            CHECK((field_at(a, p) < 0.0f) == (field_at(b, p) < 0.0f));
        }
}

TEST_CASE("cut: the sweep covers the region being cut") {
    // The frame sits well outside the block, so a sweep sized to the frame
    // rather than the region would never reach it.
    auto item = cut::cut_item(front_frame(-8.0f), CutShape::rect(0.3f, 0.3f), kRegion);
    REQUIRE(item.has_value());
    scene::Document doc = block_with(item, scene::Op::Subtract);
    CHECK(field_at(doc, cf3(0, 0, -0.99f)) > 0.0f);  // through the near face
    CHECK(field_at(doc, cf3(0, 0, 0.99f)) > 0.0f);   // and out the far one

    SUBCASE("an explicit extent cuts only that far") {
        CutOptions partial;
        // From the frame at z = -8, reach only to z = 0: a deliberate partial
        // cut, which is the only reason to set this by hand.
        partial.near_extent = 0.0f;
        partial.far_extent = 8.0f;
        auto shallow = cut::cut_item(front_frame(-8.0f), CutShape::rect(0.3f, 0.3f), kRegion,
                                     partial);
        REQUIRE(shallow.has_value());
        scene::Document shallow_doc = block_with(shallow, scene::Op::Subtract);
        CHECK(field_at(shallow_doc, cf3(0, 0, -0.5f)) > 0.0f);  // cut here
        CHECK(field_at(shallow_doc, cf3(0, 0, 0.5f)) < 0.0f);   // still solid beyond
    }
}

// Keep-inner and keep-outer are the op, not a parameter of the cut.
TEST_CASE("cut: subtract and intersect are complementary") {
    auto item = cut::cut_item(front_frame(), CutShape::circle(0.5f), kRegion);
    REQUIRE(item.has_value());
    scene::Document removed = block_with(item, scene::Op::Subtract);
    scene::Document kept = block_with(item, scene::Op::Intersect);

    int inside_one = 0, inside_both = 0, inside_neither = 0;
    for (float x = -0.9f; x <= 0.9f; x += 0.13f)
        for (float y = -0.9f; y <= 0.9f; y += 0.17f) {
            kernel::cfloat3 p = cf3(x, y, 0.0f);
            bool a = field_at(removed, p) < 0.0f;
            bool b = field_at(kept, p) < 0.0f;
            inside_one += (a != b) ? 1 : 0;
            inside_both += (a && b) ? 1 : 0;
            inside_neither += (!a && !b) ? 1 : 0;
        }
    CHECK(inside_one > 0);
    CHECK(inside_both == 0);      // no point survives both
    CHECK(inside_neither == 0);   // and inside the block, none is lost by both
}

TEST_CASE("cut: rounding bevels the walls") {
    CutOptions bevelled;
    bevelled.rounding = 0.15f;
    auto sharp = cut::cut_item(front_frame(), CutShape::rect(0.4f, 0.4f), kRegion);
    auto soft = cut::cut_item(front_frame(), CutShape::rect(0.4f, 0.4f), kRegion, bevelled);
    REQUIRE(sharp.has_value());
    REQUIRE(soft.has_value());
    CHECK(soft->rounding == doctest::Approx(0.15f));

    // A rounded cutter is fatter, so it takes more material at the wall.
    scene::Document a = block_with(sharp, scene::Op::Subtract);
    scene::Document b = block_with(soft, scene::Op::Subtract);
    CHECK(field_at(b, cf3(0.45f, 0.0f, 0.0f)) > field_at(a, cf3(0.45f, 0.0f, 0.0f)));
}

TEST_CASE("cut: a closed curve is a cut shape") {
    // Four control points around a square. As a spline the outline bulges past
    // the control polygon, so the hole is wider than the polygon's would be.
    std::vector<scene::StrokePoint> control;
    for (kernel::cfloat2 v : {cf2(-0.5f, 0.0f), cf2(0.0f, 0.5f), cf2(0.5f, 0.0f),
                              cf2(0.0f, -0.5f)}) {
        scene::StrokePoint p;
        p.pos = cf3(v.x, v.y, 0.0f);
        p.type = scene::StrokePointType::Spline;
        control.push_back(p);
    }
    CutShape spline = CutShape::from_curve(control, 0.005f);
    CHECK(spline.polygon.size() > control.size());  // it really tessellated

    CutShape straight = CutShape::from_polygon({cf2(-0.5f, 0.0f), cf2(0.0f, 0.5f),
                                                cf2(0.5f, 0.0f), cf2(0.0f, -0.5f)});
    auto smooth_item = cut::cut_item(front_frame(), spline, kRegion);
    auto hard_item = cut::cut_item(front_frame(), straight, kRegion);
    REQUIRE(smooth_item.has_value());
    REQUIRE(hard_item.has_value());

    scene::Document smooth = block_with(smooth_item, scene::Op::Subtract);
    scene::Document hard = block_with(hard_item, scene::Op::Subtract);
    // Between the two outlines: the chord between two control points passes
    // through (-0.25, 0.25) and the closed spline bulges out to
    // (-0.3125, 0.3125), so a point at (-0.28, 0.28) is inside the spline and
    // outside the straight polygon. On the spline itself the field would read
    // zero and the test would be measuring rounding, not the outline.
    kernel::cfloat3 bulge = cf3(-0.28f, 0.28f, 0.0f);
    CHECK(field_at(hard, bulge) < 0.0f);
    CHECK(field_at(smooth, bulge) > 0.0f);
}

TEST_CASE("cut: a cut is an ordinary edit") {
    scene::Document doc;
    const scene::LayerId layer = doc.add_sdf_layer("l").id;
    scene::Node solid;
    solid.prim = scene::Prim::box(cf3(1, 1, 1));
    solid.id = doc.find_layer(layer)->sdf->reserve_id();
    doc.find_layer(layer)->sdf->insert(std::move(solid));
    std::vector<std::uint8_t> before = scene::serialize_document(doc);

    auto item = cut::cut_item(front_frame(), CutShape::circle(0.4f), kRegion);
    REQUIRE(item.has_value());
    scene::Node placed = *item;
    placed.op = scene::Op::Subtract;
    placed.id = doc.find_layer(layer)->sdf->reserve_id();

    scene::UndoStack undo;
    REQUIRE(undo.perform(doc, scene::Command{scene::AddNodeCmd{layer, scene::kNoNode, -1,
                                                               {placed}}}));
    CHECK(field_at(doc, cf3(0, 0, 0)) > 0.0f);
    REQUIRE(undo.undo(doc));
    CHECK(field_at(doc, cf3(0, 0, 0)) < 0.0f);
    CHECK(scene::serialize_document(doc) == before);

    SUBCASE("a protected layer refuses it") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, false, true}});
        CHECK_FALSE(scene::apply(doc, scene::Command{scene::AddNodeCmd{layer, scene::kNoNode, -1,
                                                                       {placed}}}));
        CHECK(field_at(doc, cf3(0, 0, 0)) < 0.0f);
    }
}

TEST_CASE("cut: degenerate cuts are refused") {
    CHECK_FALSE(cut::cut_item(front_frame(), CutShape::rect(0.0f, 0.4f), kRegion).has_value());
    CHECK_FALSE(cut::cut_item(front_frame(), CutShape::circle(0.0f), kRegion).has_value());
    CHECK_FALSE(cut::cut_item(front_frame(), CutShape::from_polygon({}), kRegion).has_value());
    CHECK_FALSE(cut::cut_item(front_frame(),
                              CutShape::from_polygon({cf2(0, 0), cf2(1, 1)}), kRegion)
                    .has_value());

    // A frame whose basis is not orthonormal: the shape the user drew was not
    // in the frame they think they have, so it is refused rather than fixed.
    CutFrame skewed = front_frame();
    skewed.up = cf3(0.5f, 0.5f, 0.0f);
    CHECK_FALSE(skewed.is_orthonormal());
    CHECK_FALSE(cut::cut_item(skewed, CutShape::rect(0.4f, 0.4f), kRegion).has_value());

    CutFrame unnormalized = front_frame();
    unnormalized.right = cf3(2, 0, 0);
    CHECK_FALSE(cut::cut_item(unnormalized, CutShape::rect(0.4f, 0.4f), kRegion).has_value());
}

TEST_CASE("cut: an angled frame cuts along its own sweep") {
    // Looking down -Y instead: the same call, a different plane, and the hole
    // runs top to bottom rather than front to back.
    CutFrame from_above;
    from_above.origin = cf3(0, 4, 0);
    from_above.right = cf3(1, 0, 0);
    from_above.up = cf3(0, 0, 1);
    from_above.forward = cf3(0, -1, 0);
    REQUIRE(from_above.is_orthonormal());

    auto item = cut::cut_item(from_above, CutShape::rect(0.3f, 0.3f), kRegion);
    REQUIRE(item.has_value());
    scene::Document doc = block_with(item, scene::Op::Subtract);
    CHECK(field_at(doc, cf3(0, 0.9f, 0)) > 0.0f);
    CHECK(field_at(doc, cf3(0, -0.9f, 0)) > 0.0f);
    CHECK(field_at(doc, cf3(0.8f, 0.0f, 0.8f)) < 0.0f);
}

TEST_CASE("cut: from_basis really is the rotation it claims") {
    // The cut's orientation rests on this, so it is checked directly rather
    // than only through a cut's shape.
    kernel::cfloat3 right = cf3(0, 0, 1), up = cf3(1, 0, 0), forward = cf3(0, 1, 0);
    math::Quat q = math::Quat::from_basis(right, up, forward);
    auto close = [](kernel::cfloat3 a, kernel::cfloat3 b) {
        return kernel::clength(a - b) < 1e-5f;
    };
    CHECK(close(q.rotate(cf3(1, 0, 0)), right));
    CHECK(close(q.rotate(cf3(0, 1, 0)), up));
    CHECK(close(q.rotate(cf3(0, 0, 1)), forward));

    // A 180-degree turn is the case a naive trace-only derivation divides by
    // zero on.
    math::Quat flip = math::Quat::from_basis(cf3(-1, 0, 0), cf3(0, 1, 0), cf3(0, 0, -1));
    CHECK(close(flip.rotate(cf3(1, 0, 0)), cf3(-1, 0, 0)));
    CHECK(close(flip.rotate(cf3(0, 0, 1)), cf3(0, 0, -1)));
}
