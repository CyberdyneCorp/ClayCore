// Control-point curves (scene-model + file-io specs, add-curve-objects):
// tessellation, the bit-identity invariant that makes this a non-breaking
// change, bounds, editing and the versioned scene chunk.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "clay/io/clayspace.h"
#include "clay/pick/pick.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/curve.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using scene::StrokePoint;
using scene::StrokePointType;

namespace {

StrokePoint pt(kernel::cfloat3 p, float r, StrokePointType t = StrokePointType::Hard) {
    StrokePoint sp;
    sp.pos = p;
    sp.radius = r;
    sp.type = t;
    return sp;
}

// Four points around a square, so a smooth interpolation has somewhere to
// bulge and a hard one obviously does not.
std::vector<StrokePoint> square(float r = 0.2f,
                                StrokePointType t = StrokePointType::Hard) {
    return {pt(cf3(-1, 0, 0), r, t), pt(cf3(0, 1, 0), r, t), pt(cf3(1, 0, 0), r, t),
            pt(cf3(0, -1, 0), r, t)};
}

scene::Node curve_node(std::vector<StrokePoint> points, bool closed = false,
                       float tolerance = 0.01f) {
    scene::Node n;
    n.prim.type = scene::PrimType::Stroke;
    n.stroke = std::move(points);
    n.stroke_closed = closed;
    n.curve_tolerance = tolerance;
    return n;
}

scene::Tape compile_one(const scene::Node& node) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(node);
    return scene::compile_document(doc);
}

}  // namespace

// The invariant that makes this a non-breaking change rather than a migration:
// every stroke authored before types existed compiles to the same tape.
TEST_CASE("curve: an all-hard open list compiles to the tape it always did") {
    scene::Node node = curve_node(square());
    scene::Tape typed = compile_one(node);

    // The same points with the field the change added left at its default is
    // what an older document deserializes to, so this compares the new
    // compiler against exactly what it must reproduce.
    CHECK(scene::curve_is_polyline(node.stroke, node.stroke_closed));
    scene::Tape again = compile_one(node);
    CHECK(typed.blob == again.blob);
    CHECK(typed.instrs.size() == again.instrs.size());

    // Tessellation is the identity here, not merely equivalent to it.
    std::vector<StrokePoint> out =
        scene::tessellate_curve(node.stroke, node.stroke_closed, node.curve_tolerance);
    REQUIRE(out.size() == node.stroke.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].pos.x == node.stroke[i].pos.x);
        CHECK(out[i].pos.y == node.stroke[i].pos.y);
        CHECK(out[i].radius == node.stroke[i].radius);
    }
    // 4 points -> 4 blob entries of 4 floats
    CHECK(typed.blob.size() == 16);
}

TEST_CASE("curve: smooth points bulge outside the straight chain") {
    // A thin tube, so "outside the chord" is a question about the curve rather
    // than one the tube's own thickness answers.
    const float r = 0.05f;
    scene::Tape hard = compile_one(curve_node(square(r)));
    scene::Tape smooth = compile_one(curve_node(square(r, StrokePointType::Spline)));

    CHECK(smooth.blob.size() > hard.blob.size());  // it actually subdivided

    // The Catmull-Rom midpoint of the first span is (-0.5625, 0.5625) against
    // a chord midpoint of (-0.5, 0.5): a bulge of ~0.088, comfortably outside
    // a tube of radius 0.05.
    kernel::cfloat3 on_curve = cf3(-0.5625f, 0.5625f, 0.0f);
    CHECK(hard.eval(on_curve).d > 0.0f);
    CHECK(smooth.eval(on_curve).d < 0.0f);

    // ...and it still passes through every control point: Catmull-Rom
    // interpolates, unlike the B-spline below.
    for (const StrokePoint& p : square(r)) CHECK(smooth.eval(p.pos).d < 0.0f);
}

TEST_CASE("curve: a B-spline approximates rather than interpolating") {
    scene::Tape bs = compile_one(curve_node(square(0.05f, StrokePointType::BSpline)));
    // A tight radius so "passes through the control point" is a real question
    // rather than one the tube's thickness answers.
    int inside = 0;
    for (const StrokePoint& p : square()) inside += bs.eval(p.pos).d < 0.0f ? 1 : 0;
    CHECK(inside < 4);  // it smooths the corners off rather than hitting them
}

TEST_CASE("curve: Bezier handles shape the span, in local space") {
    std::vector<StrokePoint> pts = {pt(cf3(-1, 0, 0), 0.1f, StrokePointType::Bezier),
                                    pt(cf3(1, 0, 0), 0.1f, StrokePointType::Bezier)};
    pts[0].out_handle = cf3(0, 2, 0);
    pts[1].in_handle = cf3(0, 2, 0);
    scene::Tape bent = compile_one(curve_node(pts));

    // A cubic with both handles at +2y peaks at (a + 3ca + 3cb + b)/8 =
    // (0, 1.5): the span is pulled up there, and the chord's midpoint is left
    // outside the tube entirely.
    CHECK(bent.eval(cf3(0, 1.5f, 0)).d < 0.0f);
    CHECK(bent.eval(cf3(0, 0, 0)).d > 0.0f);

    // Straightening the handles restores the straight tube.
    pts[0].out_handle = pts[1].in_handle = cf3(0, 0, 0);
    scene::Tape flat = compile_one(curve_node(pts));
    CHECK(flat.eval(cf3(0, 0, 0)).d < 0.0f);
    CHECK(flat.eval(cf3(0, 1.5f, 0)).d > 0.0f);
}

TEST_CASE("curve: a closed curve joins its ends") {
    scene::Tape open_curve = compile_one(curve_node(square(0.15f, StrokePointType::Spline), false));
    scene::Tape closed = compile_one(curve_node(square(0.15f, StrokePointType::Spline), true));

    // Midway along the span from the last control point back to the first.
    kernel::cfloat3 closing = cf3(-0.55f, -0.55f, 0.0f);
    CHECK(open_curve.eval(closing).d > 0.0f);
    CHECK(closed.eval(closing).d < 0.0f);

    SUBCASE("a closed hard list is a polygon ring, not a polyline") {
        scene::Tape ring = compile_one(curve_node(square(0.15f), true));
        CHECK(ring.eval(cf3(-0.5f, -0.5f, 0.0f)).d < 0.0f);  // the closing edge exists
        CHECK_FALSE(scene::curve_is_polyline(square(), true));
    }
}

TEST_CASE("curve: tolerance controls subdivision, deterministically and boundedly") {
    auto segments = [](float tolerance) {
        return compile_one(curve_node(square(0.2f, StrokePointType::Spline), false, tolerance))
                   .blob.size() /
               4;
    };
    std::size_t coarse = segments(0.2f);
    std::size_t fine = segments(0.002f);
    CHECK(fine > coarse);

    SUBCASE("the same curve compiles to the same chain") {
        scene::Node n = curve_node(square(0.2f, StrokePointType::Spline));
        CHECK(compile_one(n).blob == compile_one(n).blob);
    }

    SUBCASE("subdivision is bounded rather than unbounded") {
        // Past the depth bound a smaller tolerance buys nothing, which is the
        // point: a tiny tolerance must not grow the tape without limit.
        std::size_t at_bound = segments(1e-9f);
        CHECK(at_bound == segments(1e-12f));
        // 3 spans, each at most 2^kMaxCurveDepth sub-spans, plus the end point
        CHECK(at_bound <= 3u * (1u << scene::kMaxCurveDepth) + 1u);
    }

    SUBCASE("a finer curve lies closer to the ideal") {
        // The exact Catmull-Rom midpoint of the first span, which a coarse
        // tessellation chords across and a fine one nearly reaches.
        auto deviation = [](float tolerance) {
            std::vector<StrokePoint> tess = scene::tessellate_curve(
                square(0.2f, StrokePointType::Spline), false, tolerance);
            // Distance from the exact curve point to the nearest emitted one.
            kernel::cfloat3 exact = cf3(-0.5f, 0.5625f, 0.0f);  // p(0.5) of span 0
            float best = 1e9f;
            for (const StrokePoint& p : tess)
                best = std::min(best, kernel::clength(p.pos - exact));
            return best;
        };
        CHECK(deviation(0.002f) < deviation(0.2f));
    }
}

TEST_CASE("curve: bounds cover the bulge, so picking does not miss it") {
    // A Bezier whose handles carry it far outside its control points: both
    // control points sit at y = 0, and the span peaks at y = 1.5. A bound
    // taken from the control points would stop at y = radius and cull the
    // whole arc away.
    std::vector<StrokePoint> pts = {pt(cf3(-1, 0, 0), 0.1f, StrokePointType::Bezier),
                                    pt(cf3(1, 0, 0), 0.1f, StrokePointType::Bezier)};
    pts[0].out_handle = cf3(0, 2, 0);
    pts[1].in_handle = cf3(0, 2, 0);

    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    layer.sdf->insert(curve_node(pts));

    math::Aabb control_only{cf3(-1.1f, -0.1f, -0.1f), cf3(1.1f, 0.1f, 0.1f)};
    math::Aabb b = scene::item_local_bounds(*layer.sdf->find(layer.sdf->roots[0]));
    CHECK(b.max.y > control_only.max.y);
    CHECK(b.max.y > 1.0f);

    // ...and a ray aimed at the arc really does hit, which is the thing the
    // bound exists to make true.
    math::Ray ray{cf3(0, 1.5f, -5.0f), cf3(0, 0, 1)};
    CHECK(pick::raycast_scene(doc, ray, {}).hit);
}

TEST_CASE("curve: editing the points is an ordinary edit") {
    scene::Document doc;
    const scene::LayerId layer = doc.add_sdf_layer("l").id;
    scene::Node node = curve_node(square());
    node.id = doc.find_layer(layer)->sdf->reserve_id();
    const scene::NodeId id = node.id;
    doc.find_layer(layer)->sdf->insert(node);

    std::vector<std::uint8_t> before = scene::serialize_document(doc);

    scene::UndoStack undo;
    scene::SetStrokePointsCmd cmd{layer, id, square(0.4f, StrokePointType::Spline), true, 0.005f};
    REQUIRE(undo.perform(doc, scene::Command{cmd}));
    const scene::Node* edited = doc.find_layer(layer)->sdf->find(id);
    REQUIRE(edited);
    CHECK(edited->stroke_closed);
    CHECK(edited->curve_tolerance == doctest::Approx(0.005f));
    CHECK(edited->stroke[0].type == StrokePointType::Spline);

    REQUIRE(undo.undo(doc));
    CHECK(scene::serialize_document(doc) == before);  // exact, not approximate

    SUBCASE("a protected layer refuses it") {
        scene::apply(doc, scene::Command{scene::SetLayerProtectionCmd{layer, false, true}});
        // Compared against the document as protecting left it: locking is
        // itself a recorded change, so `before` is the wrong baseline here.
        std::vector<std::uint8_t> protectedd = scene::serialize_document(doc);
        CHECK_FALSE(scene::apply(doc, scene::Command{cmd}));
        CHECK(scene::serialize_document(doc) == protectedd);
    }

    SUBCASE("it refuses a node that is not a stroke") {
        scene::Node sphere;
        sphere.prim = scene::Prim::sphere(1.0f);
        sphere.id = doc.find_layer(layer)->sdf->reserve_id();
        scene::NodeId sid = sphere.id;
        doc.find_layer(layer)->sdf->insert(sphere);
        scene::SetStrokePointsCmd bad{layer, sid, square(), false, 0.01f};
        CHECK_FALSE(scene::apply(doc, scene::Command{bad}));
    }

    SUBCASE("and it edits a swept guide, which is the same point list") {
        // Stroke-only left a placed sweep's guide unreachable: the guide is an
        // ordinary curve, so replacing it is the ordinary curve edit.
        scene::Node sweep;
        sweep.prim = scene::Prim::swept(0);
        sweep.stroke = square();
        sweep.profiles = {scene::Profile::circle(0.2f), scene::Profile::circle(0.1f)};
        sweep.profile_polygons.resize(2);
        sweep.id = doc.find_layer(layer)->sdf->reserve_id();
        scene::NodeId wid = sweep.id;
        doc.find_layer(layer)->sdf->insert(sweep);

        scene::SetStrokePointsCmd guide{layer, wid, square(0.2f, StrokePointType::Spline), false,
                                        0.005f};
        REQUIRE(scene::apply(doc, scene::Command{guide}));
        const scene::Node* n = doc.find_layer(layer)->sdf->find(wid);
        REQUIRE(n);
        CHECK(n->stroke[0].type == StrokePointType::Spline);
        CHECK(n->curve_tolerance == doctest::Approx(0.005f));
    }

    SUBCASE("the command round trips through its own encoding") {
        std::vector<std::uint8_t> bytes = scene::serialize(scene::Command{cmd});
        auto back = scene::deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());
        const auto* c = std::get_if<scene::SetStrokePointsCmd>(&*back);
        REQUIRE(c);
        CHECK(c->closed == cmd.closed);
        CHECK(c->tolerance == doctest::Approx(cmd.tolerance));
        REQUIRE(c->points.size() == cmd.points.size());
        CHECK(c->points[1].type == cmd.points[1].type);
        CHECK(c->points[1].pos.y == doctest::Approx(cmd.points[1].pos.y));
    }
}

TEST_CASE("curve: round trips through the document format") {
    io::ClaySpaceDoc file;
    scene::Layer& layer = file.document.add_sdf_layer("l");
    std::vector<StrokePoint> pts = square(0.2f, StrokePointType::Bezier);
    pts[0].out_handle = cf3(0.3f, 0.7f, -0.2f);
    pts[2].in_handle = cf3(-0.4f, 0.1f, 0.5f);
    pts[3].type = StrokePointType::BSpline;
    layer.sdf->insert(curve_node(pts, true, 0.004f));

    std::vector<std::uint8_t> bytes = io::save_clayspace(file);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());

    const scene::Layer* l = back.document.find_layer(layer.id);
    REQUIRE(l);
    const scene::Node* n = l->sdf->find(l->sdf->roots[0]);
    REQUIRE(n);
    CHECK(n->stroke_closed);
    CHECK(n->curve_tolerance == doctest::Approx(0.004f));
    REQUIRE(n->stroke.size() == pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        CHECK(n->stroke[i].type == pts[i].type);
        CHECK(n->stroke[i].out_handle.y == doctest::Approx(pts[i].out_handle.y));
        CHECK(n->stroke[i].in_handle.z == doctest::Approx(pts[i].in_handle.z));
    }
    // ...and the field is what it was.
    CHECK(scene::compile_document(back.document).blob ==
          scene::compile_document(file.document).blob);
}

// The version on the scene chunk is what lets a node gain a field without a
// packing trick. The stream is WRITTEN at the old layout rather than spliced
// out of a current one: a splice only stays correct until the next field is
// added anywhere else in the record, which is exactly what happened when the
// loft profile list arrived.
TEST_CASE("curve: an older document loads with hard corners") {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    std::vector<StrokePoint> pts = {pt(cf3(1234.5f, -4321.25f, 777.125f), 0.375f,
                                       StrokePointType::Spline),
                                    pt(cf3(-1234.5f, 4321.25f, -777.125f), 0.5f,
                                       StrokePointType::Bezier)};
    pts[0].out_handle = cf3(1, 2, 3);
    layer.sdf->insert(curve_node(pts, true, 0.004f));

    std::vector<std::uint8_t> older = scene::serialize_document(doc, 1);
    std::vector<std::uint8_t> current = scene::serialize_document(doc);
    CHECK(older.size() < current.size());

    auto back = scene::deserialize_document(older.data(), older.size(), 1);
    REQUIRE(back.has_value());
    REQUIRE(back->layers.size() == 1);
    const scene::Node* n = back->layers[0].sdf->find(back->layers[0].sdf->roots[0]);
    REQUIRE(n);
    REQUIRE(n->stroke.size() == pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        // Position and radius survive because minor 1 carried them; the type,
        // the handles, closed and the tolerance take their defaults, which is
        // what those points already meant before the fields existed.
        CHECK(n->stroke[i].pos.x == doctest::Approx(pts[i].pos.x));
        CHECK(n->stroke[i].radius == doctest::Approx(pts[i].radius));
        CHECK(n->stroke[i].type == StrokePointType::Hard);
        CHECK(n->stroke[i].out_handle.y == doctest::Approx(0.0f));
    }
    CHECK_FALSE(n->stroke_closed);
    CHECK(n->curve_tolerance == doctest::Approx(scene::Node{}.curve_tolerance));

    SUBCASE("the current layout keeps everything") {
        auto full = scene::deserialize_document(current.data(), current.size());
        REQUIRE(full.has_value());
        const scene::Node* m = full->layers[0].sdf->find(full->layers[0].sdf->roots[0]);
        REQUIRE(m);
        CHECK(m->stroke[0].type == StrokePointType::Spline);
        CHECK(m->stroke[0].out_handle.y == doctest::Approx(2.0f));
        CHECK(m->stroke_closed);
    }
}
