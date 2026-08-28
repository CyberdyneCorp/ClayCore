// The consolidation policy (scene-model spec, add-consolidation-policy), and
// the redistancing pass it rests on.
//
// The claims under test are the two the change exists to make true: that a
// chain of region-verb edits holds its declared Lipschitz within a stated
// bound instead of multiplying per edit, and that collapsing one is a single
// undo step whose inverse gives back the parametric form.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "clay/field/flatten.h"
#include "clay/field/redistance.h"
#include "clay/field/volume.h"
#include "clay/kernel/exactness.h"
#include "clay/eval/bake_points.h"
#include "clay/eval/bake_volume.h"
#include "clay/field/relax.h"
#include "clay/field/move_topological.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/tape.h"

using namespace clay;
using field::FieldVolume;
using kernel::cf3;

namespace {

// The bound this change states. sqrt(3) is cfi_volume's interpolation factor,
// which a volume pays whatever its samples do; the slack above it is what a
// redistanced bake is allowed to measure over a perfect 1.
constexpr float kConsolidatedSampleBound = 1.10f;
constexpr float kConsolidatedDeclaredBound = 1.7320508f * kConsolidatedSampleBound;

scene::Document sphere_document(float radius) {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::sphere(radius);
    layer.sdf->insert(n);
    return doc;
}

// One hPolish pass: plane the form back to `distance` along `normal`, cutting
// only, over a region with a taper — the shape of gesture that does not chain.
FieldVolume polish(const scene::Document& source, kernel::cfloat3 normal, float distance,
                   float cell, float band) {
    const scene::Tape tape = scene::compile_document(source);
    field::FlattenSettings s;
    s.plane_point = normal * distance;
    s.plane_normal = normal;
    s.strength = 1.0f;
    s.centre = normal * distance;
    s.region_radius = 0.7f;
    s.falloff = 0.3f;
    s.mode = field::FlattenMode::CutOnly;
    const kernel::cfloat3 pad = cf3(band, band, band);
    return field::flatten([&tape](kernel::cfloat3 p) { return tape.eval(p).d; },
                          math::Aabb{tape.bounds.min - pad, tape.bounds.max + pad}, cell, band, s);
}

scene::Document wrap(const FieldVolume& v) {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("f");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const FieldVolume>(v);
    layer.sdf->insert(n);
    return doc;
}

// Where the surface sits along a direction, by marching a fine ruler outwards.
float surface_along(const scene::Document& doc, kernel::cfloat3 u, float from) {
    const scene::Tape tape = scene::compile_document(doc);
    for (float t = from; t > 0.0f; t -= 0.002f)
        if (tape.eval(u * t).d <= 0.0f) return t;
    return -1.0f;
}

scene::ConsolidationParams params_at(float cell, float band) {
    scene::ConsolidationParams p;
    p.cell_size = cell;
    p.band = band;
    return p;
}

}  // namespace

// -- redistancing --------------------------------------------------------------

TEST_CASE("redistance replaces a steep field with the distance to its own surface") {
    // A sphere, deliberately steepened by a factor of eight. Its zero set is
    // exactly the sphere's; only the values around it are wrong.
    auto steep = [](kernel::cfloat3 p) { return (kernel::clength(p) - 0.6f) * 8.0f; };
    FieldVolume v = FieldVolume::sample(steep, math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}, 0.02f,
                                        0.16f);
    REQUIRE(v.measure_sample_lipschitz() > 5.0f);

    REQUIRE(field::redistance(v));
    CHECK(v.measure_sample_lipschitz() <= kConsolidatedSampleBound);
    CHECK(v.sample_lipschitz() == doctest::Approx(v.measure_sample_lipschitz()));

    // The surface did not move: the interface is found by interpolating
    // between the samples that bracket it, which is where it already was.
    for (kernel::cfloat3 u : {cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1),
                              kernel::cnormalize(cf3(1, 1, 1))}) {
        CHECK(v.eval(u * 0.6f) == doctest::Approx(0.0f).epsilon(0.0).scale(1.0f).epsilon(0.05));
        CHECK(v.eval(u * 0.5f) < 0.0f);
        CHECK(v.eval(u * 0.7f) > 0.0f);
    }
    // And the values are now distances rather than merely signed: half way in
    // from the surface the field reads about that.
    CHECK(v.eval(cf3(0.5f, 0, 0)) == doctest::Approx(-0.1f).epsilon(0.15));
}

TEST_CASE("redistance leaves a field with no zero set alone") {
    // Wholly outside the sampled box: nothing changes sign, so there is no
    // interface to measure from and inventing one would be worse than the
    // bound the sampling already produced.
    auto far_away = [](kernel::cfloat3 p) { return kernel::clength(p - cf3(9, 9, 9)) - 0.5f; };
    FieldVolume v = FieldVolume::sample(far_away, math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)},
                                        0.05f, 0.15f);
    CHECK_FALSE(field::redistance(v));
}

TEST_CASE("compact drops the bricks a redistanced field shows are past the band") {
    auto ball = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.6f; };
    FieldVolume v = FieldVolume::sample(ball, math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}, 0.02f,
                                        0.1f);
    const std::size_t before = v.brick_count();
    REQUIRE(field::redistance(v));
    CHECK(v.compact() > 0);
    CHECK(v.brick_count() < before);
    // Dropping them may not move the surface: the claim compact rests on is
    // that a brick whose samples are all past the band holds no crossing.
    CHECK(v.eval(cf3(0.6f, 0, 0)) == doctest::Approx(0.0f).epsilon(0.0).scale(1.0f).epsilon(0.05));
    CHECK(v.eval(cf3(0.55f, 0, 0)) < 0.0f);
    CHECK(v.eval(cf3(0.65f, 0, 0)) > 0.0f);
}

TEST_CASE("the reported byte cost is the blob's real length") {
    // blob_floats() exists so that asking what a volume costs does not
    // materialise a copy of every sample. The two can only drift silently, so
    // they are checked against each other.
    auto ball = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.6f; };
    FieldVolume v = FieldVolume::sample(ball, math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}, 0.04f,
                                        0.16f);
    CHECK(v.blob_floats() == v.to_blob().size());
    REQUIRE(field::redistance(v));
    v.compact();
    CHECK(v.blob_floats() == v.to_blob().size());
}

// -- the soundness bug the policy would otherwise inherit ------------------------

TEST_CASE("a sampled volume never declares a bound smaller than its samples measure") {
    // FieldVolume::sample used to leave sample_lipschitz at 1 whatever it had
    // just stored, which meant baking a steep chain declared it 1-Lipschitz —
    // licensing exactly the overstep the declared bound exists to prevent.
    auto steep = [](kernel::cfloat3 p) { return (kernel::clength(p) - 0.6f) * 14.0f; };
    const FieldVolume v = FieldVolume::sample(steep, math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)},
                                              0.02f, 0.2f);
    CHECK(v.sample_lipschitz() >= v.measure_sample_lipschitz() - 1e-3f);
    CHECK(v.sample_lipschitz() > 5.0f);

    // A field that IS 1-Lipschitz still measures 1, so nothing honest pays.
    const FieldVolume plain = FieldVolume::sample(
        [](kernel::cfloat3 p) { return kernel::clength(p) - 0.6f; },
        math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}, 0.02f, 0.2f);
    CHECK(plain.sample_lipschitz() == doctest::Approx(1.0f).epsilon(0.02));
}

// -- the chain the change exists for ---------------------------------------------

TEST_CASE("a chain of polish passes degrades, and consolidation bounds it") {
    const float cell = 0.03f, band = 0.12f;
    const kernel::cfloat3 dirs[3] = {cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1)};

    SUBCASE("unconsolidated, the declared Lipschitz multiplies per pass") {
        scene::Document cur = sphere_document(0.62f);
        std::vector<float> declared;
        for (int i = 0; i < 3; ++i) {
            cur = wrap(polish(cur, dirs[i], 0.44f, cell, band));
            declared.push_back(scene::compile_document(cur).info.lipschitz);
        }
        CHECK(declared[0] <= kConsolidatedDeclaredBound);  // one pass is clean
        CHECK(declared[1] > declared[0] * 5.0f);           // the second is not
        CHECK(declared[2] > kConsolidatedDeclaredBound * 4.0f);
    }

    SUBCASE("consolidated between passes, it holds the stated bound") {
        scene::Document cur = sphere_document(0.62f);
        for (int i = 0; i < 4; ++i) {
            cur = wrap(polish(cur, dirs[i % 3], 0.44f, cell, band));
            scene::ConsolidationCost cost;
            REQUIRE(scene::consolidate_layer(cur, cur.layers.front().id, params_at(cell, band),
                                             nullptr, &cost));
            INFO("pass " << i + 1 << " sample lipschitz " << cost.sample_lipschitz);
            CHECK(cost.sample_lipschitz <= kConsolidatedSampleBound);
            CHECK(cost.lipschitz <= kConsolidatedDeclaredBound);
            CHECK(scene::compile_document(cur).info.lipschitz <= kConsolidatedDeclaredBound);
            // And the memory does not creep: compacting after redistancing is
            // what stops each bake keeping the shell of bricks the last one's
            // band bound put just outside it.
            CHECK(cost.bytes < 6u * 1024u * 1024u);
        }
        // The facet is still on its plane after four passes.
        CHECK(surface_along(cur, dirs[0], 1.2f) == doctest::Approx(0.44f).epsilon(0.06));
    }
}

TEST_CASE("a move stroke stops decaying once it is consolidated") {
    scene::Document doc = sphere_document(1.0f);
    scene::Layer& layer = doc.layers.front();
    const scene::NodeId id = layer.sdf->roots.front();

    // Nine drags, each a grab on the chain: the decay is geometric because
    // deformer_lipschitz multiplies them.
    for (int i = 0; i < 9; ++i)
        layer.sdf->find_mut(id)->deformers.insert(
            layer.sdf->find_mut(id)->deformers.begin(),
            scene::Deformer::grab(cf3(1.0f + 0.25f * static_cast<float>(i), 0, 0), 0.5f,
                                  cf3(0.25f, 0, 0)));
    const scene::FieldReport before = scene::report_layer(layer, 0.25f);
    CHECK(before.longest_deformer_chain == 9);
    CHECK(before.steepest_volume == doctest::Approx(1.0f));  // no volume is involved at all
    CHECK(before.safe_step_scale < 0.05f);
    CHECK(before.advises_consolidation);

    scene::ConsolidationCost cost;
    REQUIRE(scene::consolidate_layer(doc, layer.id, params_at(0.03f, 0.12f), nullptr, &cost));
    const scene::FieldReport after = scene::report_layer(doc.layers.front(), 0.25f);
    CHECK(after.longest_deformer_chain == 0);
    CHECK(after.lipschitz <= kConsolidatedDeclaredBound);
    CHECK(after.safe_step_scale > before.safe_step_scale * 10.0f);
    CHECK_FALSE(after.advises_consolidation);
}

TEST_CASE("the report names which of the two mechanisms degraded a chain") {
    // A steep VOLUME and a long deformer chain cost the same aggregate and
    // want different cures, so an aggregate alone cannot tell an app which to
    // offer.
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const FieldVolume>(FieldVolume::sample(
        [](kernel::cfloat3 p) { return (kernel::clength(p) - 0.6f) * 9.0f; },
        math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}, 0.03f, 0.15f));
    layer.sdf->insert(n);

    const scene::FieldReport report = scene::report_layer(layer, 0.25f);
    CHECK(report.steepest_volume > 5.0f);
    CHECK(report.longest_deformer_chain == 0);
    CHECK(report.item_count == 1);
    CHECK(report.advises_consolidation);
    // Advice only when asked for: a threshold of zero measures without judging.
    CHECK_FALSE(scene::report_layer(layer, 0.0f).advises_consolidation);
}

// -- one undoable command ---------------------------------------------------------

TEST_CASE("consolidation undoes exactly to the parametric form") {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node a;
    a.prim = scene::Prim::sphere(0.7f);
    a.color = cf3(0.9f, 0.1f, 0.1f);
    scene::Node b;
    b.prim = scene::Prim::box(cf3(0.3f, 0.3f, 0.3f));
    b.op = scene::Op::Subtract;
    b.xform.position = cf3(0.5f, 0, 0);
    scene::Node c;
    c.prim = scene::Prim::sphere(0.25f);
    c.xform.position = cf3(0, 0.7f, 0);
    c.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.15f};
    const scene::NodeId ia = layer.sdf->insert(a);
    const scene::NodeId ib = layer.sdf->insert(b);
    const scene::NodeId ic = layer.sdf->insert(c);
    const std::vector<scene::NodeId> before = layer.sdf->roots;

    scene::UndoStack undo;
    REQUIRE(scene::consolidate_layer(doc, layer.id, params_at(0.03f, 0.12f), &undo));
    CHECK(doc.layers.front().sdf->roots.size() == 1);
    CHECK(scene::consolidation_state(doc.layers.front()));
    // ONE step, however many items it absorbed.
    CHECK(undo.undo_depth() == 1);

    REQUIRE(undo.undo(doc));
    const scene::SdfContent& back = *doc.layers.front().sdf;
    CHECK(back.roots == before);  // same ids, same order
    CHECK_FALSE(scene::consolidation_state(doc.layers.front()));

    // Editable by their parameters again, which is the whole claim.
    REQUIRE(back.find(ia) != nullptr);
    CHECK(back.find(ia)->prim.type == scene::PrimType::Sphere);
    CHECK(back.find(ia)->prim.params[0] == doctest::Approx(0.7f));
    CHECK(back.find(ia)->color.x == doctest::Approx(0.9f));
    CHECK(back.find(ib)->op == scene::Op::Subtract);
    CHECK(back.find(ib)->xform.position.x == doctest::Approx(0.5f));
    CHECK(back.find(ic)->blend.k == doctest::Approx(0.15f));

    // And redo puts the bake back, still as one step.
    REQUIRE(undo.redo(doc));
    CHECK(doc.layers.front().sdf->roots.size() == 1);
    CHECK(scene::consolidation_state(doc.layers.front()));
}

TEST_CASE("consolidation is refused on a protected layer") {
    for (bool lock : {true, false}) {
        scene::Document doc = sphere_document(0.6f);
        scene::Layer& layer = doc.layers.front();
        layer.locked = lock;
        layer.ghost = !lock;
        const std::vector<scene::NodeId> before = layer.sdf->roots;
        CHECK_FALSE(scene::consolidate_layer(doc, layer.id, params_at(0.05f, 0.15f)));
        CHECK(doc.layers.front().sdf->roots == before);
    }
}

TEST_CASE("consolidation leaves hidden items alone") {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node visible;
    visible.prim = scene::Prim::sphere(0.6f);
    scene::Node hidden;
    hidden.prim = scene::Prim::sphere(0.3f);
    hidden.visible = false;
    layer.sdf->insert(visible);
    const scene::NodeId ih = layer.sdf->insert(hidden);

    REQUIRE(scene::consolidate_layer(doc, layer.id, params_at(0.04f, 0.15f)));
    const scene::SdfContent& after = *doc.layers.front().sdf;
    CHECK(after.roots.size() == 2);
    REQUIRE(after.find(ih) != nullptr);
    CHECK(after.find(ih)->prim.params[0] == doctest::Approx(0.3f));
    // Still parametric, because a parametric item is still there.
    CHECK_FALSE(scene::consolidation_state(doc.layers.front()));
}

// -- what a host may still promise --------------------------------------------------

TEST_CASE("consolidation state reports the resolution, and only for a baked layer") {
    scene::Document doc = sphere_document(0.6f);
    CHECK_FALSE(scene::consolidation_state(doc.layers.front()));

    scene::ConsolidationCost paid;
    REQUIRE(scene::consolidate_layer(doc, doc.layers.front().id, params_at(0.04f, 0.16f), nullptr,
                                     &paid));
    scene::ConsolidationCost seen;
    REQUIRE(scene::consolidation_state(doc.layers.front(), &seen));
    CHECK(seen.cell_size == doctest::Approx(0.04f));
    CHECK(seen.band == doctest::Approx(0.16f));
    CHECK(seen.brick_count == paid.brick_count);
    CHECK(seen.bytes == paid.bytes);

    // A layer holding a volume among other items is NOT consolidated: the
    // other items still have parameters an app can offer.
    scene::Node extra;
    extra.prim = scene::Prim::sphere(0.2f);
    doc.layers.front().sdf->insert(extra);
    CHECK_FALSE(scene::consolidation_state(doc.layers.front()));
}

TEST_CASE("the cost is knowable before it is paid, and the document does not change") {
    scene::Document doc = sphere_document(0.6f);
    const std::vector<std::uint8_t> before = scene::serialize_document(doc);

    scene::ConsolidationCost quoted;
    REQUIRE(scene::bake_layer(doc.layers.front(), params_at(0.04f, 0.16f), &quoted));
    CHECK(quoted.brick_count > 0);
    CHECK(quoted.bytes > 0);
    CHECK(quoted.sample_count > 0);
    CHECK(scene::serialize_document(doc) == before);

    scene::ConsolidationCost paid;
    REQUIRE(scene::consolidate_layer(doc, doc.layers.front().id, params_at(0.04f, 0.16f), nullptr,
                                     &paid));
    // The quote IS the bill: the estimate is the real bake with the result
    // thrown away, so it cannot drift from what the real one produces.
    CHECK(paid.brick_count == quoted.brick_count);
    CHECK(paid.bytes == quoted.bytes);
    CHECK(paid.sample_lipschitz == doctest::Approx(quoted.sample_lipschitz));
}

TEST_CASE("a document that never consolidates is untouched by the policy") {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("l");
    scene::Node a;
    a.prim = scene::Prim::sphere(0.7f);
    scene::Node b;
    b.prim = scene::Prim::torus(0.5f, 0.15f);
    b.op = scene::Op::Subtract;
    layer.sdf->insert(a);
    layer.sdf->insert(b);

    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    const scene::Tape tape = scene::compile_document(doc);

    // Asking costs nothing: reporting, quoting a cost and checking the state
    // are all reads, and none of them is allowed to leave a trace.
    scene::report_layer(doc.layers.front(), 0.5f);
    scene::ConsolidationCost cost;
    scene::bake_layer(doc.layers.front(), params_at(0.05f, 0.15f), &cost);
    scene::consolidation_state(doc.layers.front());

    CHECK(scene::serialize_document(doc) == bytes);
    const scene::Tape again = scene::compile_document(doc);
    CHECK(again.instrs.size() == tape.instrs.size());
    CHECK(again.params == tape.params);
    CHECK(again.info.lipschitz == doctest::Approx(tape.info.lipschitz));
}

TEST_CASE("a hidden layer still reports and still consolidates") {
    // compile_layer treats a hidden layer as empty, which would report a
    // degraded chain as clean and refuse to bake it — making "hide the layer"
    // a way to get stuck in a state nothing will tell you about.
    scene::Document doc = sphere_document(0.6f);
    scene::Layer& layer = doc.layers.front();
    layer.sdf->find_mut(layer.sdf->roots.front())
        ->deformers.push_back(scene::Deformer::grab(cf3(0.6f, 0, 0), 0.4f, cf3(0.2f, 0, 0)));
    const scene::FieldReport shown = scene::report_layer(layer, 0.9f);
    layer.visible = false;
    const scene::FieldReport hidden = scene::report_layer(doc.layers.front(), 0.9f);
    CHECK(hidden.lipschitz == doctest::Approx(shown.lipschitz));
    CHECK(hidden.advises_consolidation == shown.advises_consolidation);
    CHECK(hidden.longest_deformer_chain == 1);

    REQUIRE(scene::consolidate_layer(doc, layer.id, params_at(0.03f, 0.12f)));
    CHECK_FALSE(doc.layers.front().visible);  // consolidating does not unhide it
    CHECK(scene::consolidation_state(doc.layers.front()));
}

TEST_CASE("consolidating keeps the layer's own transform rather than baking it twice") {
    scene::Document doc = sphere_document(0.5f);
    scene::Layer& layer = doc.layers.front();
    layer.xform.position = cf3(2.0f, 0, 0);
    layer.xform.scale = 2.0f;
    const float before = surface_along(doc, cf3(1, 0, 0), 4.0f);
    REQUIRE(before > 0.0f);

    REQUIRE(scene::consolidate_layer(doc, layer.id, params_at(0.03f, 0.12f)));
    CHECK(doc.layers.front().xform.position.x == doctest::Approx(2.0f));
    CHECK(surface_along(doc, cf3(1, 0, 0), 4.0f) == doctest::Approx(before).epsilon(0.05));
}

// -- the bake's grid path -----------------------------------------------------
//
// The bake batches every lattice sample through the CPU backend's pool
// (src/scene/consolidate.cpp). The contract is byte-identity with the serial
// full-tape bake it replaced: these tests hold the whole pipeline — sampling,
// classification, redistance, compact, the re-measured bound — to it.
//
// The dabbed scene below is the one that keeps the bake honest about
// per-brick CULLED tapes: its chain of quadratic blends carries a culled
// item's tail into band-interior values (up to ~7e-3 measured), so a bake
// that reached for the refill path's culling would fail these byte
// comparisons. See the grid-path comment in consolidate.cpp.

namespace {

// A layer whose field exercises brick classification everywhere it can go:
// smooth-blended dabs, a subtract, and empty bricks on both sides of the
// surface.
scene::Document dabbed_document() {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("sculpt");
    scene::Node base;
    base.prim = scene::Prim::sphere(0.8f);
    layer.sdf->insert(base);
    for (int i = 0; i < 24; ++i) {
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.12f);
        dab.blend.profile = scene::BlendProfile::Quadratic;
        dab.blend.k = 0.06f;
        const float a = static_cast<float>(i) * 0.7f;
        dab.xform.position =
            cf3(0.8f * std::cos(a), 0.8f * std::sin(a), 0.25f * std::sin(a * 1.9f));
        layer.sdf->insert(dab);
    }
    scene::Node bite;
    bite.prim = scene::Prim::sphere(0.35f);
    bite.op = scene::Op::Subtract;
    bite.xform.position = cf3(0.0f, 0.75f, 0.2f);
    layer.sdf->insert(bite);
    return doc;
}

// The serial reference: bake_layer's exact steps with the FULL layer tape at
// every sample — the path the grid bake replaced, kept here as the yardstick.
FieldVolume serial_bake(const scene::Layer& layer, const scene::ConsolidationParams& params) {
    scene::Layer view = layer;
    view.visible = true;
    view.xform = math::Transform{};
    const scene::Tape tape = scene::compile_layer(view);
    REQUIRE(!tape.empty());
    const float band = params.band > 0.0f ? params.band : params.cell_size * 3.0f;
    const float padding = params.padding > 0.0f ? params.padding : band;
    math::Aabb region = params.region;
    if (region.empty()) {
        const kernel::cfloat3 pad = cf3(padding, padding, padding);
        region = math::Aabb{tape.bounds.min - pad, tape.bounds.max + pad};
    }
    FieldVolume v = FieldVolume::sample([&tape](kernel::cfloat3 p) { return tape.eval(p).d; },
                                        region, params.cell_size, band);
    if (!params.skip_redistance && field::redistance(v)) v.compact();
    // The bake fills colour after redistance and compact, so the reference has
    // to as well — this test's whole point is that the pooled grid bake and a
    // serial full-tape bake produce the SAME BYTES, and a channel present in
    // one and absent in the other is a difference in the bytes.
    //
    // Including WHETHER it fills one: a layer of one colour gets no channel,
    // and the rule comes from scene::layer_colors_vary rather than being
    // restated here, so the reference cannot drift from the bake it checks.
    if (scene::layer_colors_vary(layer))
        v.fill_colors([&tape](kernel::cfloat3 p) { return tape.eval(p).color; });
    v.set_sample_lipschitz(v.measure_sample_lipschitz());
    return v;
}

}  // namespace

TEST_CASE("the grid bake is byte-identical to the serial full-tape bake") {
    scene::Document doc = dabbed_document();
    scene::ConsolidationParams params = params_at(0.05f, 0.15f);

    // With redistance skipped first: the raw baked samples compared directly,
    // where a repair pass that missed a sample cannot hide behind the solve.
    params.skip_redistance = true;
    std::optional<FieldVolume> raw =
        scene::bake_layer(doc.layers.front(), params, nullptr, eval::pooled_bake_eval());
    REQUIRE(raw);
    CHECK(raw->serialize() == serial_bake(doc.layers.front(), params).serialize());

    // And the full pipeline, redistance and compact included.
    params.skip_redistance = false;
    std::optional<FieldVolume> baked =
        scene::bake_layer(doc.layers.front(), params, nullptr, eval::pooled_bake_eval());
    REQUIRE(baked);
    CHECK(baked->serialize() == serial_bake(doc.layers.front(), params).serialize());
}

TEST_CASE("the grid bake matches the serial bake on a steep volume chain") {
    // A polish pass hands back a STEEP volume (that is why chains consolidate
    // at all), so kept bricks hold values far past the band — the raw stores
    // that make the bake stricter than any band-clamped consumer.
    scene::Document doc = wrap(polish(sphere_document(0.8f), cf3(0, 0, 1), 0.55f, 0.02f, 0.08f));
    scene::ConsolidationParams params = params_at(0.04f, 0.12f);
    std::optional<FieldVolume> baked =
        scene::bake_layer(doc.layers.front(), params, nullptr, eval::pooled_bake_eval());
    REQUIRE(baked);
    CHECK(baked->serialize() == serial_bake(doc.layers.front(), params).serialize());
}

TEST_CASE("the pooled tape fill bakes the volume the per-point tape callable does") {
    // What the document-sourced verbs in the bindings changed to. `sample`
    // asks a tape for one point at a time; `tape_block_fill` hands the CPU
    // backend a window and lets it spread the window across its pool. The
    // contract is byte-identity, not a tolerance — eval_points writes to
    // disjoint slices and each point goes through the same scalar reference
    // arithmetic — so a difference of one ULP is a defect, not rounding.
    //
    // A polished sphere rather than a plain one: it is a STEEP field, so kept
    // bricks hold values far past the band, and a fill that disagreed about a
    // sample near a brick's edge would change which bricks survive rather than
    // only what they hold.
    scene::Document doc = wrap(polish(sphere_document(0.8f), cf3(0, 0, 1), 0.55f, 0.02f, 0.08f));
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const float cell = 0.04f, band = 0.12f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    auto per_point = [&tape](kernel::cfloat3 p) { return tape.eval(p).d; };

    SUBCASE("the plain bake") {
        const FieldVolume serial = FieldVolume::sample(per_point, region, cell, band);
        const FieldVolume pooled =
            FieldVolume::sample_blocks(eval::tape_block_fill(tape), region, cell, band);
        REQUIRE(serial.brick_count() > 0);
        CHECK(serial.serialize() == pooled.serialize());
    }

    SUBCASE("relax from a document") {
        field::RelaxSettings settings;
        settings.strength = 0.5f;
        settings.radius_cells = 2;
        settings.iterations = 2;
        settings.centre = cf3(0, 0, 0.6f);
        settings.region_radius = 0.3f;
        settings.falloff = 0.1f;
        const FieldVolume serial = field::relax(per_point, region, cell, band, settings);
        const FieldVolume pooled =
            field::relax(eval::tape_block_fill(tape), region, cell, band, settings);
        REQUIRE(serial.brick_count() > 0);
        CHECK(serial.serialize() == pooled.serialize());
    }

    SUBCASE("flatten from a document") {
        // Flatten is the one that could not simply bake first and operate
        // after: it blends toward the plane INSIDE the sampled callable, and
        // sample_blocks decides which bricks to keep from the values it is
        // handed. The batched form applies the blend to the block the source
        // filled, which is what keeps the two brick sets the same one.
        field::FlattenSettings settings;
        settings.plane_point = cf3(0, 0, 0);
        settings.plane_normal = cf3(0, 0, 1);
        settings.strength = 0.5f;
        settings.centre = cf3(0, 0, 0.7f);
        settings.region_radius = 0.3f;
        settings.falloff = 0.1f;
        settings.mode = field::FlattenMode::TwoSided;
        const FieldVolume serial = field::flatten(per_point, region, cell, band, settings);
        const FieldVolume pooled =
            field::flatten(eval::tape_block_fill(tape), region, cell, band, settings);
        REQUIRE(serial.brick_count() > 0);
        CHECK(serial.brick_count() == pooled.brick_count());
        CHECK(serial.serialize() == pooled.serialize());
    }

    SUBCASE("flatten with settings that describe no flatten") {
        // The guard both source overloads share: a zero normal, or no region,
        // means the source is sampled unchanged. Worth pinning because the
        // batched overload takes that branch through a different function.
        field::FlattenSettings settings;
        settings.plane_normal = cf3(0, 0, 0);
        settings.strength = 1.0f;
        settings.region_radius = 0.3f;
        const FieldVolume serial = field::flatten(per_point, region, cell, band, settings);
        const FieldVolume pooled =
            field::flatten(eval::tape_block_fill(tape), region, cell, band, settings);
        CHECK(serial.serialize() == pooled.serialize());
        CHECK(serial.serialize() == FieldVolume::sample(per_point, region, cell, band).serialize());
    }
}

TEST_CASE("the per-brick culled bake is the whole-tape bake, byte for byte") {
    // Culling the bake's tape per brick is not a tolerance, it is exact, and
    // the reason is two facts about culling rather than a measurement:
    //
    //   culled >= true, because culling drops items from a minimum; and
    //   culled <= band => true <= band => the two are EQUAL, because an item is
    //   only dropped when its bound is more than a band from the brick.
    //
    // So a sample the culled tape puts inside the band is already the truth; a
    // brick with no such sample stores nothing, and only its side is read; and
    // the rest -- the samples a KEPT brick stores beyond the band -- are paid
    // for with the whole tape. Storing culled values there instead would make
    // the volume overstate its own distance by 1.65 cells against the plain
    // bake's 0.1, which is a field a marcher steps through.
    const float cell = 0.04f, band = 0.12f;

    auto compare = [&](const scene::Document& doc, const char* what) {
        CAPTURE(what);
        const scene::Tape tape = scene::compile_document(doc);
        REQUIRE(!tape.empty());
        const kernel::cfloat3 pad = cf3(band, band, band);
        const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
        const FieldVolume plain =
            FieldVolume::sample_blocks(eval::tape_block_fill(tape), region, cell, band);
        const FieldVolume culled =
            FieldVolume::sample_blocks(eval::document_block_fill(doc, tape), region, cell, band);
        REQUIRE(plain.brick_count() > 0);
        CHECK(plain.brick_count() == culled.brick_count());
        CHECK(plain.sample_lipschitz() == culled.sample_lipschitz());
        CHECK(plain.serialize() == culled.serialize());
    };

    // Spread items: the case culling exists for, where a brick needs a handful
    // of a long tape.
    auto sphere_of_dabs = [&](float k, int nodes) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("s");
        scene::Node base;
        base.prim = scene::Prim::sphere(1.0f);
        l.sdf->insert(base);
        for (int i = 1; i < nodes; ++i) {
            scene::Node dab;
            dab.prim = scene::Prim::sphere(0.05f);
            if (k > 0.0f) dab.blend = scene::Blend{scene::BlendProfile::Quadratic, k};
            const float a = 0.3f * std::sin(static_cast<float>(i) * 0.7f);
            const float b = 0.3f * std::cos(static_cast<float>(i) * 1.3f);
            dab.xform.position =
                cf3(-std::sqrt(std::max(0.0f, 1.0f - a * a - b * b)), a, b);
            l.sdf->insert(dab);
        }
        return doc;
    };

    // A hard union culls hardest, so it is where a wrong value would show.
    compare(sphere_of_dabs(0.0f, 120), "hard union, 120 items");
    // A blend keeps more per brick and eventually makes culling a loss, at
    // which point the fill falls back to the whole tape — which must ALSO give
    // the same volume, since that is the same code path the plain bake takes.
    compare(sphere_of_dabs(0.05f, 120), "smooth union, 120 items");
    compare(sphere_of_dabs(0.30f, 120), "smooth union so wide the cull is refused");
    // One item: nothing to cull, and the probe must not divide by zero or
    // decide anything silly on a tape of two instructions.
    {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("one");
        scene::Node n;
        n.prim = scene::Prim::sphere(0.6f);
        l.sdf->insert(n);
        compare(doc, "a single sphere");
    }
    // Subtraction, so the sign of a sample-free brick is decided by something
    // other than a plain union.
    {
        scene::Document doc = sphere_of_dabs(0.0f, 40);
        scene::Node hole;
        hole.prim = scene::Prim::sphere(0.45f);
        hole.op = scene::Op::Subtract;
        doc.layers.front().sdf->insert(hole);
        compare(doc, "with a subtracted void");
    }
}

TEST_CASE("the pooled point batch moves the volume the per-point source does") {
    // move_topological could not join the batched bake when the other verbs did
    // (#271), and the reason is in the query positions rather than in the
    // arithmetic: where an output sample takes its material from is the
    // PULLED-BACK point, which depends on the geodesic weight there. So this
    // takes a batch of arbitrary points rather than a fill that knows the
    // lattice, and both places the source is asked anything go through it --
    // the material array the walk runs on, and the sampling pass.
    //
    // Byte-identity, not a tolerance: the batched evaluator slices its output
    // disjointly and each point goes through the same scalar reference
    // arithmetic.
    scene::Document doc = wrap(polish(sphere_document(0.8f), cf3(0, 0, 1), 0.55f, 0.02f, 0.08f));
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const float cell = 0.04f, band = 0.12f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    auto per_point = [&tape](kernel::cfloat3 p) { return tape.eval(p).d; };

    field::TopologicalMoveSettings settings;
    settings.anchor = cf3(0, 0, 0.8f);
    settings.radius = 0.3f;
    settings.displacement = cf3(0.0f, 0.08f, 0.0f);
    settings.ease = 0;

    SUBCASE("a drag that moves material") {
        const FieldVolume serial =
            field::move_topological(per_point, region, cell, band, settings);
        const FieldVolume pooled =
            field::move_topological(eval::tape_point_batch(tape), region, cell, band, settings);
        REQUIRE(serial.brick_count() > 0);
        CHECK(serial.serialize() == pooled.serialize());
    }

    SUBCASE("a drag that moves nothing takes the other door") {
        // Zero displacement returns the source sampled unchanged, and the two
        // overloads reach that through different functions -- so it is pinned
        // rather than assumed.
        field::TopologicalMoveSettings none = settings;
        none.displacement = cf3(0, 0, 0);
        const FieldVolume serial = field::move_topological(per_point, region, cell, band, none);
        const FieldVolume pooled =
            field::move_topological(eval::tape_point_batch(tape), region, cell, band, none);
        CHECK(serial.serialize() == pooled.serialize());
        CHECK(serial.serialize() == FieldVolume::sample(per_point, region, cell, band).serialize());
    }

    SUBCASE("an anchor with no material within reach") {
        // solve() gives up before the walk, which is a third path through the
        // split between make_grid and solve_over.
        field::TopologicalMoveSettings away = settings;
        away.anchor = cf3(4.0f, 4.0f, 4.0f);
        const FieldVolume serial = field::move_topological(per_point, region, cell, band, away);
        const FieldVolume pooled =
            field::move_topological(eval::tape_point_batch(tape), region, cell, band, away);
        CHECK(serial.serialize() == pooled.serialize());
    }
}

TEST_CASE("a batched fill's order does not change the volume") {
    // More bricks than one fill window, filled back to front within each
    // window: sample_blocks assembles in slot order regardless, so the volume
    // is the one the serial fill builds, byte for byte.
    auto f = [](kernel::cfloat3 p) { return kernel::clength(p) - 2.0f; };
    const math::Aabb region{cf3(-3, -3, -3), cf3(3, 3, 3)};
    const float cell = 0.06f, band = 0.2f;
    FieldVolume serial = FieldVolume::sample(f, region, cell, band);
    FieldVolume batched = FieldVolume::sample_blocks(
        [&f](const FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
             float* out) {
            for (std::size_t s = count; s-- > 0;)
                for (int i = 0; i < field::kBrickSamples; ++i)
                    out[s * field::kBrickSamples + i] = f(grid.sample_position(first + s, i));
        },
        region, cell, band);
    CHECK(serial.brick_count() > 0);
    CHECK(serial.serialize() == batched.serialize());
}

// -- surface colour (decide-surface-colour) -----------------------------------
// The two properties the surface-colour decision rests on. Both were verified
// by hand against this build before the decision was written; pinning them here
// is what stops the decision quietly becoming false.

TEST_CASE("painting colours a surface without moving it") {
    // scene::Op::Paint is what makes polypaint on an SDF layer possible at all: a
    // stroke of paint items stains the accumulated colour and leaves the
    // distance alone, so colour is not a shape edit.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    body.color = cf3(0.69f, 0.69f, 0.69f);
    l.sdf->insert(body);

    const scene::Tape plain = compile_document(doc);

    scene::Node stain;
    stain.prim = scene::Prim::sphere(0.18f);
    stain.xform.position = cf3(0, 0, 1.02f);
    stain.op = scene::Op::Paint;
    stain.color = cf3(0.878f, 0.188f, 0.125f);
    l.sdf->insert(stain);
    const scene::Tape painted = compile_document(doc);

    // The DISTANCE is untouched everywhere, exactly.
    // A deterministic lattice rather than a random walk: this is an exactness
    // claim, so the points it is made at should not vary between runs.
    for (int i = 0; i < 512; ++i) {
        const kernel::cfloat3 p =
            cf3(static_cast<float>(i % 8 - 4) * 0.45f, static_cast<float>((i / 8) % 8 - 4) * 0.45f,
                static_cast<float>((i / 64) % 8 - 4) * 0.45f);
        REQUIRE(painted.eval(p).d == plain.eval(p).d);
    }

    // ...and the colour under the stamp is the one that was authored, rather
    // than a blend with the item beneath it.
    const kernel::CTapeValue at = painted.eval(cf3(0, 0, 1.0f));
    CHECK(at.color.x == doctest::Approx(0.878f));
    CHECK(at.color.y == doctest::Approx(0.188f));
    CHECK(at.color.z == doctest::Approx(0.125f));
    // Well away from the stamp it is the body's own colour.
    const kernel::CTapeValue away = painted.eval(cf3(0, 0, -1.0f));
    CHECK(away.color.x == doctest::Approx(0.69f));
}

TEST_CASE("consolidation preserves painted colour exactly") {
    // The property the whole surface-colour story rests on: colour resolution
    // moves from ITEM-bound to TEXEL-bound by baking, and an artist does not
    // repaint to get there. Consolidation is advertised as changing cost rather
    // than appearance, and colour is part of appearance.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    body.color = cf3(0.69f, 0.69f, 0.69f);
    l.sdf->insert(body);
    for (int i = 0; i < 5; ++i) {
        scene::Node stain;
        stain.prim = scene::Prim::sphere(0.18f);
        stain.xform.position = cf3(-0.4f + 0.2f * static_cast<float>(i), 0, 1.02f);
        stain.op = scene::Op::Paint;
        stain.color = cf3(0.878f, 0.188f, 0.125f);
        l.sdf->insert(stain);
    }

    const scene::Tape before = compile_document(doc);
    std::vector<kernel::cfloat3> probes;
    for (int i = 0; i < 3; ++i)
        probes.push_back(cf3(-0.4f + 0.4f * static_cast<float>(i), 0, 1.0f));
    probes.push_back(cf3(0, 0, -1.0f));
    probes.push_back(cf3(1.0f, 0, 0));
    std::vector<kernel::cfloat3> was;
    for (const kernel::cfloat3& p : probes) was.push_back(before.eval(p).color);

    scene::ConsolidationParams params;
    params.cell_size = 0.02f;
    REQUIRE(scene::consolidate_layer(doc, l.id, params));

    const scene::Tape after = compile_document(doc);
    for (std::size_t i = 0; i < probes.size(); ++i) {
        CAPTURE(i);
        const kernel::cfloat3 now = after.eval(probes[i]).color;
        CHECK(now.x == doctest::Approx(was[i].x).epsilon(0.02));
        CHECK(now.y == doctest::Approx(was[i].y).epsilon(0.02));
        CHECK(now.z == doctest::Approx(was[i].z).epsilon(0.02));
    }
}

// -- installing a volume the caller already has (sdf-sculpt-transaction spec) --
//
// Consolidation is two things sold together: sample the layer, then replace its
// edit list with the result. A live Smooth stroke has already done the first
// half, once, at pointer-down; making it bake the layer AGAIN at pointer-up to
// reach the installer would throw away the whole reason it held the volume. So
// the installer is a function, and it is the SAME code — these hold it to that.

TEST_CASE("consolidate: installing a volume is exactly what consolidate_layer installs") {
    scene::Document baked = sphere_document(0.6f);
    scene::Document installed = sphere_document(0.6f);
    const scene::ConsolidationParams params = params_at(0.04f, 0.12f);

    scene::ConsolidationCost baked_cost, installed_cost;
    REQUIRE(scene::consolidate_layer(baked, baked.layers.front().id, params, nullptr,
                                     &baked_cost));

    std::optional<FieldVolume> v =
        scene::bake_layer(installed.layers.front(), params, nullptr);
    REQUIRE(v);
    REQUIRE(scene::replace_layer_with_volume(installed, installed.layers.front().id,
                                             std::move(*v), nullptr, &installed_cost));

    // Same document, byte for byte: same node ids, same colour, same samples.
    CHECK(scene::serialize_document(installed) == scene::serialize_document(baked));
    CHECK(installed_cost.brick_count == baked_cost.brick_count);
    CHECK(installed_cost.sample_lipschitz == baked_cost.sample_lipschitz);
    CHECK(scene::consolidation_state(installed.layers.front()));
}

TEST_CASE("consolidate: installing a volume is one undo step, and its inverse is exact") {
    scene::Document doc = sphere_document(0.6f);
    scene::UndoStack undo;
    const std::vector<std::uint8_t> before = scene::serialize_document(doc);

    std::optional<FieldVolume> v =
        scene::bake_layer(doc.layers.front(), params_at(0.04f, 0.12f), nullptr);
    REQUIRE(v);
    REQUIRE(scene::replace_layer_with_volume(doc, doc.layers.front().id, std::move(*v), &undo));

    CHECK(undo.undo_depth() == 1);
    const std::vector<std::uint8_t> after = scene::serialize_document(doc);
    REQUIRE(undo.undo(doc));
    CHECK(scene::serialize_document(doc) == before);  // the sphere's radius is back
    REQUIRE(undo.redo(doc));
    CHECK(scene::serialize_document(doc) == after);
}

TEST_CASE("consolidate: installing a volume refuses a protected layer") {
    scene::Document doc = sphere_document(0.6f);
    std::optional<FieldVolume> v =
        scene::bake_layer(doc.layers.front(), params_at(0.04f, 0.12f), nullptr);
    REQUIRE(v);
    const std::vector<std::uint8_t> before = scene::serialize_document(doc);

    doc.layers.front().locked = true;
    CHECK_FALSE(scene::replace_layer_with_volume(doc, doc.layers.front().id, *v, nullptr));
    doc.layers.front().locked = false;
    doc.layers.front().ghost = true;
    CHECK_FALSE(scene::replace_layer_with_volume(doc, doc.layers.front().id, *v, nullptr));
    doc.layers.front().ghost = false;

    CHECK_FALSE(scene::replace_layer_with_volume(doc, 9999, *v, nullptr));
    CHECK(scene::serialize_document(doc) == before);
}

TEST_CASE("consolidate: installing a volume severs shared instance content") {
    scene::Document doc = sphere_document(0.6f);
    const scene::LayerId src = doc.layers.front().id;
    scene::Layer* inst = doc.instance_layer(src, "copy");
    REQUIRE(inst != nullptr);
    // .get() on both sides: a shared_ptr inside an assertion is a Windows-only
    // build error, because MSVC's <memory> declares an operator<< for one that
    // doctest's stringifier then cannot deduce against.
    REQUIRE(inst->sdf.get() == doc.layers.front().sdf.get());

    std::optional<FieldVolume> v =
        scene::bake_layer(doc.layers.front(), params_at(0.04f, 0.12f), nullptr);
    REQUIRE(v);
    scene::UndoStack undo;
    REQUIRE(scene::replace_layer_with_volume(doc, src, std::move(*v), &undo));

    // Nine subtools must not collapse because the artist baked the tenth.
    const scene::Layer* instance = doc.find_layer(doc.layers.back().id);
    REQUIRE(instance != nullptr);
    REQUIRE(instance->sdf->roots.size() == 1);
    CHECK(instance->sdf->find(instance->sdf->roots.front())->prim.type ==
          scene::PrimType::Sphere);
    CHECK(scene::consolidation_state(*doc.find_layer(src)));
    // ...and one undo puts back both the absorbed item and the sharing.
    CHECK(undo.undo_depth() == 1);
    REQUIRE(undo.undo(doc));
    CHECK(doc.find_layer(src)->sdf.get() == doc.layers.back().sdf.get());
}

// -- the pure tape half of a bake (add-sdf-prefix-cache) -----------------------
//
// `bake_layer` is now `compile a local view` plus `bake_tape`, so that a caller
// holding a tape that belongs to no layer — the prefix cache samples exactly
// that — gets the same sampling, redistance, compact, colour and measurement
// without a second definition of what a baked volume is. These hold the two to
// being the same code rather than two that agree today.

TEST_CASE("consolidate: bake_tape reproduces bake_layer byte for byte") {
    scene::Document doc = sphere_document(0.6f);
    const scene::ConsolidationParams params = params_at(0.04f, 0.12f);
    const scene::Layer& layer = doc.layers.front();

    std::optional<FieldVolume> through_layer = scene::bake_layer(layer, params);
    REQUIRE(through_layer);

    // The caller's half of the contract, spelled out: the layer's own frame,
    // visible, and the colour question asked of the NODES.
    scene::Layer view = layer;
    view.visible = true;
    view.xform = math::Transform{};
    const scene::Tape tape = scene::compile_layer(view);
    std::optional<FieldVolume> through_tape =
        scene::bake_tape(tape, params, scene::layer_colors_vary(layer));
    REQUIRE(through_tape);

    CHECK(through_tape->serialize() == through_layer->serialize());
}

TEST_CASE("consolidate: bake_tape carries colour on exactly the same rule") {
    // Two documents that differ only in whether the layer can produce more than
    // one colour. `want_color` is the caller's to supply because the tape has
    // already folded colour into instructions and cannot be asked; passing it
    // wrongly is different BYTES, not a slower path, which is what this pins.
    scene::Document grey = sphere_document(0.6f);
    scene::Document painted = sphere_document(0.6f);
    scene::Node red;
    red.prim = scene::Prim::sphere(0.2f);
    red.xform.position = cf3(0.5f, 0, 0);
    red.color = cf3(1, 0, 0);
    painted.layers.front().sdf->insert(red);

    CHECK_FALSE(scene::layer_colors_vary(grey.layers.front()));
    CHECK(scene::layer_colors_vary(painted.layers.front()));

    const scene::ConsolidationParams params = params_at(0.04f, 0.12f);
    for (scene::Document* d : {&grey, &painted}) {
        scene::Layer view = d->layers.front();
        view.visible = true;
        view.xform = math::Transform{};
        const scene::Tape tape = scene::compile_layer(view);
        const bool vary = scene::layer_colors_vary(d->layers.front());
        std::optional<FieldVolume> want = scene::bake_layer(d->layers.front(), params);
        std::optional<FieldVolume> got = scene::bake_tape(tape, params, vary);
        REQUIRE(want);
        REQUIRE(got);
        CHECK(got->has_color() == vary);
        CHECK(got->serialize() == want->serialize());
    }
}

TEST_CASE("consolidate: bake_tape refuses what bake_layer refuses") {
    scene::Document doc = sphere_document(0.6f);
    scene::Layer view = doc.layers.front();
    view.visible = true;
    view.xform = math::Transform{};
    const scene::Tape tape = scene::compile_layer(view);

    scene::ConsolidationParams no_cell;  // cell_size stays 0
    CHECK_FALSE(scene::bake_tape(tape, no_cell, false));

    const scene::Tape empty;  // no instructions: nothing to sample
    CHECK_FALSE(scene::bake_tape(empty, params_at(0.04f, 0.12f), false));

    // ...and a cancelled token discards rather than returning a partial bake.
    parallel::CancelToken token;
    token.cancel();
    CHECK_FALSE(scene::bake_tape(tape, params_at(0.04f, 0.12f), false, nullptr, {}, &token));
}
