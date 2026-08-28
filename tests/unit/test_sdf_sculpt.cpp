// Transient SDF sculpt transactions (sdf-sculpt-transaction spec): a live
// Smooth that changes its preview before pointer-up, a live Move that never
// writes to the document while the pointer is down, and a session policy that
// keeps repeated sculpting bounded without the engine deciding to bake.
//
// The first thing every test here asserts is a NEGATIVE: that the persistent
// document did not change. That is the whole architecture, and a preview that
// looks right while quietly mutating the document is the defect this replaces.

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "clay/eval/bake_points.h"
#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/scene/commands.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/session/sdf_sculpt.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;
using namespace clay::scene;
using session::SdfMoveTransaction;
using session::SdfSculptDirty;
using session::SdfSculptPolicy;
using session::SdfSmoothTransaction;

namespace {

const float kCell = 0.05f;

SdfSculptPolicy smooth_policy() {
    SdfSculptPolicy p;
    p.cell_size = kCell;
    return p;
}

// A blobby form with a feature small enough for a dab to move: two balls
// smooth-unioned, which is also the case a naive Move gets wrong.
Document two_balls(float gap = 0.45f) {
    Document doc;
    Layer& layer = doc.add_sdf_layer("body");
    for (float x : {-gap, gap}) {
        Node ball;
        ball.prim = Prim::sphere(0.5f);
        ball.op = Op::Add;
        ball.blend = Blend{BlendProfile::Quadratic, 0.25f};
        ball.xform.position = cf3(x, 0, 0);
        layer.sdf->insert(ball);
    }
    return doc;
}

// The same form plus `n` unrelated items far enough away that no brush in this
// file reaches them. The scaling tests add these and assert that per-frame work
// does not notice.
Document two_balls_plus_distant(std::size_t n) {
    Document doc = two_balls();
    Layer& layer = doc.layers.front();
    for (std::size_t i = 0; i < n; ++i) {
        Node dab;
        dab.prim = Prim::sphere(0.02f);
        dab.xform.position = cf3(40.0f + 0.1f * static_cast<float>(i), 0, 0);
        layer.sdf->insert(dab);
    }
    return doc;
}

field::RelaxSettings dab(cfloat3 centre, float radius = 0.25f, float strength = 0.8f) {
    field::RelaxSettings s;
    s.centre = centre;
    s.region_radius = radius;
    s.strength = strength;
    s.radius_cells = 1;
    s.iterations = 1;
    return s;
}

// The volume a Smooth transaction starts from, taken the long way round so a
// test can relax it standalone and compare.
field::FieldVolume baked(const Layer& layer, float cell = kCell) {
    ConsolidationParams params;
    params.cell_size = cell;
    return *bake_layer(layer, params);
}

// Whether a document round-trips to the same bytes: the exact "nothing
// happened" that a pointer comparison cannot give.
std::vector<std::uint8_t> snapshot(const Document& doc) { return serialize_document(doc); }

}  // namespace

// -- 12.2 / 12.3: Smooth is online, and the document does not move ------------

TEST_CASE("sculpt: a live Smooth changes its preview and not the document") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    const std::vector<std::uint8_t> before = snapshot(doc);
    const field::FieldVolume source = baked(doc.layers.front());

    auto tx = SdfSmoothTransaction::begin(doc, id, smooth_policy());
    REQUIRE(tx);
    CHECK(snapshot(doc) == before);  // begin() alone must not edit anything

    // The regression this feature exists for: after ONE update the preview has
    // already moved. Before the transaction, Smooth only appeared at stroke end.
    const cfloat3 seam = cf3(0, 0.42f, 0);
    const SdfSculptDirty d = tx->update(dab(seam, 0.3f));
    CHECK(d.changed);
    CHECK(d.touched_bricks > 0);
    CHECK_FALSE(d.bounds.empty());
    CHECK(tx->preview_volume().eval(seam) != source.eval(seam));

    // ...and a hundred more of them still have not touched the document.
    for (int i = 0; i < 100; ++i) tx->update(dab(seam, 0.3f));
    CHECK(snapshot(doc) == before);

    tx->cancel();
    CHECK(snapshot(doc) == before);  // 4.3: cancel is lossless
}

TEST_CASE("sculpt: a live Smooth pushes no undo steps until it commits") {
    Document doc = two_balls();
    UndoStack undo;
    auto tx = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(tx);
    for (int i = 0; i < 20; ++i) tx->update(dab(cf3(0, 0.42f, 0)));
    CHECK(undo.undo_depth() == 0);
    REQUIRE(tx->commit(&undo));
    CHECK(undo.undo_depth() == 1);  // 12.5: one gesture, one step — never twenty
}

// -- 12.4: the live sequence equals the standalone sequence -------------------

TEST_CASE("sculpt: a Smooth transaction equals the same dabs through field::relax") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;

    const field::RelaxSettings a = dab(cf3(0, 0.42f, 0), 0.30f, 0.7f);
    const field::RelaxSettings b = dab(cf3(0.2f, 0.40f, 0), 0.25f, 0.5f);
    const field::RelaxSettings c = dab(cf3(-0.2f, 0.40f, 0), 0.20f, 0.9f);

    field::FieldVolume expected = baked(doc.layers.front());
    expected = field::relax(expected, a);
    expected = field::relax(expected, b);
    expected = field::relax(expected, c);

    auto tx = SdfSmoothTransaction::begin(doc, id, smooth_policy());
    REQUIRE(tx);
    tx->update(a);
    tx->update(b);
    tx->update(c);
    // BYTE-identical: relax_in_place is the same algorithm with a different
    // owner, so anything less would mean the refactor changed the arithmetic.
    CHECK(tx->preview_volume().serialize() == expected.serialize());

    REQUIRE(tx->commit(nullptr));
    const Layer& after = doc.layers.front();
    REQUIRE(after.sdf->roots.size() == 1);
    const Node* installed = after.sdf->find(after.sdf->roots.front());
    REQUIRE(installed);
    REQUIRE(installed->volume);
    CHECK(installed->volume->serialize() == expected.serialize());
}

// -- 12.6: the source layer is evaluated once, at begin, and never again ------

TEST_CASE("sculpt: a Smooth gesture evaluates the source layer exactly once") {
    Document doc = two_balls();
    UndoStack undo;
    int evaluations = 0;
    // A counting evaluator that DECLINES: bake_layer calls it per window and
    // falls back to the serial walk, so the bytes are the bake's own while the
    // counter still sees every request. That is the seam — the volume cannot
    // change because of the instrument measuring it.
    BakePointEval counting = [&evaluations](const Tape&, const float*, std::size_t, float*,
                                            float*) {
        ++evaluations;
        return false;
    };

    auto tx = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy(), counting);
    REQUIRE(tx);
    const int after_begin = evaluations;
    CHECK(after_begin > 0);

    for (int i = 0; i < 50; ++i) tx->update(dab(cf3(0, 0.42f, 0)));
    CHECK(evaluations == after_begin);  // no update re-bakes

    REQUIRE(tx->commit(&undo));
    CHECK(evaluations == after_begin);  // and neither does the commit
}

// -- 12.5: undo and redo of a committed Smooth --------------------------------

TEST_CASE("sculpt: one Smooth commit undoes and redoes exactly") {
    Document doc = two_balls();
    UndoStack undo;
    const std::vector<std::uint8_t> before = snapshot(doc);

    auto tx = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(tx);
    for (int i = 0; i < 5; ++i) tx->update(dab(cf3(0, 0.42f, 0)));
    REQUIRE(tx->commit(&undo));

    const std::vector<std::uint8_t> committed = snapshot(doc);
    CHECK(committed != before);
    REQUIRE(undo.undo(doc));
    CHECK(snapshot(doc) == before);  // the two balls are back, parameters intact
    REQUIRE(undo.redo(doc));
    CHECK(snapshot(doc) == committed);
}

// -- 12.15: a stale transaction cannot overwrite a concurrent edit ------------

TEST_CASE("sculpt: a Smooth commit refuses a layer that changed underneath it") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    UndoStack undo;

    auto tx = SdfSmoothTransaction::begin(doc, id, smooth_policy());
    REQUIRE(tx);
    tx->update(dab(cf3(0, 0.42f, 0)));

    // Somebody else edits the layer through the ordinary command path.
    Node extra;
    extra.prim = Prim::sphere(0.2f);
    extra.xform.position = cf3(0, 1.2f, 0);
    REQUIRE(undo.perform(doc, Command{AddNodeCmd{id, kNoNode, -1, {extra}}}));
    const std::vector<std::uint8_t> external = snapshot(doc);
    const std::size_t depth = undo.undo_depth();

    CHECK_FALSE(tx->commit(&undo));
    CHECK(snapshot(doc) == external);   // the external edit survives untouched
    CHECK(undo.undo_depth() == depth);  // and no partial step was installed
    CHECK_FALSE(tx->live());
}

TEST_CASE("sculpt: a Smooth transaction refuses what it cannot own") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    CHECK_FALSE(SdfSmoothTransaction::begin(doc, 9999, smooth_policy()));  // no such layer

    SdfSculptPolicy no_cell;  // cell_size stays 0: the caller must choose one
    CHECK_FALSE(SdfSmoothTransaction::begin(doc, id, no_cell));

    doc.layers.front().locked = true;
    CHECK_FALSE(SdfSmoothTransaction::begin(doc, id, smooth_policy()));
}

TEST_CASE("sculpt: a masked Smooth freezes exactly what the mask covers") {
    Document doc = two_balls();
    const cfloat3 frozen = cf3(0, 0.42f, 0);
    const field::FieldVolume source = baked(doc.layers.front());

    field::RelaxSettings s = dab(frozen, 0.35f);
    s.mask = [](cfloat3) { return 1.0f; };  // wholly frozen

    auto tx = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(tx);
    const SdfSculptDirty d = tx->update(s);
    // The bricks are still selected — the region is geometry — but not one
    // SAMPLE in them moved, which is the difference `changed` exists to report.
    CHECK(d.touched_bricks > 0);
    CHECK_FALSE(d.changed);
    for (float y = 0.2f; y <= 0.6f; y += 0.02f)
        CHECK(tx->preview_volume().eval(cf3(0, y, 0)) == source.eval(cf3(0, y, 0)));

    // Byte-identical to a standalone masked relax, band and all: the band is
    // narrowed by what a pass COULD have moved the surface, which is a property
    // of the kernel rather than of the samples, and both paths narrow it alike.
    CHECK(tx->preview_volume().serialize() == field::relax(source, s).serialize());

    // Half a mask scales the effect rather than gating it.
    field::RelaxSettings half = dab(frozen, 0.35f);
    half.mask = [](cfloat3) { return 0.5f; };
    auto partial = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(partial);
    CHECK(partial->update(half).changed);
    CHECK(partial->preview_volume().serialize() == field::relax(source, half).serialize());
}

// -- 12.7 / 12.8: Move is transactional --------------------------------------

TEST_CASE("sculpt: a live Move leaves every persistent chain alone") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    UndoStack undo;
    const std::vector<std::uint8_t> before = snapshot(doc);

    auto tx = SdfMoveTransaction::begin(doc, id, cf3(0, 0, 0), brush::MoveSettings{0.8f, 0, false});
    REQUIRE(tx);
    REQUIRE(tx->affected_count() == 2);

    for (int i = 1; i <= 100; ++i)
        tx->update(cf3(0, 0.004f * static_cast<float>(i), 0));

    CHECK(snapshot(doc) == before);  // no SetDeformersCmd ever reached the document
    CHECK(undo.undo_depth() == 0);
    for (NodeId n : doc.layers.front().sdf->roots)
        CHECK(doc.layers.front().sdf->find(n)->deformers.empty());

    // ...while the preview did move.
    const Layer& preview = tx->preview_layer();
    for (NodeId n : preview.sdf->roots) CHECK(preview.sdf->find(n)->deformers.size() == 1);
}

TEST_CASE("sculpt: a Move preview is the TOTAL displacement, not a composition") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    const brush::MoveSettings settings{0.8f, 0, false};

    auto tx = SdfMoveTransaction::begin(doc, id, cf3(0, 0, 0), settings);
    REQUIRE(tx);
    tx->update(cf3(0.1f, 0, 0));
    tx->update(cf3(0.2f, 0, 0));
    tx->update(cf3(0.5f, 0, 0));

    // What one fresh drag of 0.5 from the untouched layer produces.
    Document fresh = two_balls();
    const std::vector<brush::MoveWarp> warps =
        brush::move_brush(fresh.layers.front(), cf3(0, 0, 0), cf3(0.5f, 0, 0), settings);
    REQUIRE(warps.size() == 2);
    for (const brush::MoveWarp& w : warps) {
        const Node* n = fresh.layers.front().sdf->find(w.node);
        REQUIRE(scene::apply(fresh, Command{SetDeformersCmd{fresh.layers.front().id, w.node,
                                                            brush::moved_chain(*n, w)}}));
    }

    const Tape live = compile_layer(tx->preview_layer());
    const Tape once = compile_layer(fresh.layers.front());
    for (float y = -1.0f; y <= 1.0f; y += 0.05f)
        for (float x = -1.0f; x <= 1.0f; x += 0.05f)
            CHECK(live.eval(cf3(x, y, 0)).d == doctest::Approx(once.eval(cf3(x, y, 0)).d));

    // A composition of the three would have moved the surface further than 0.5
    // ever asked for; the equality above is what rules it out.
    CHECK(tx->preview_layer().sdf->find(warps.front().node)->deformers.size() == 1);
}

// -- 12.9 / 14.4: preparation once, then work proportional to what moves ------

TEST_CASE("sculpt: Move per-frame work does not notice unrelated model") {
    const brush::MoveSettings settings{0.8f, 0, false};
    std::size_t small_visits = 0, large_visits = 0;
    std::size_t small_prepared = 0, large_prepared = 0;
    brush::MovePrepareStats small_prepare, large_prepare;

    for (std::size_t extra : {std::size_t{0}, std::size_t{5000}}) {
        Document doc = two_balls_plus_distant(extra);
        auto tx = SdfMoveTransaction::begin(doc, doc.layers.front().id, cf3(0, 0, 0), settings);
        REQUIRE(tx);
        for (int i = 0; i < 10; ++i) tx->update(cf3(0, 0.02f * static_cast<float>(i), 0));
        if (extra == 0) {
            small_visits = tx->last_update_visited();
            small_prepared = tx->affected_count();
            small_prepare = tx->prepare_stats();
        } else {
            large_visits = tx->last_update_visited();
            large_prepared = tx->affected_count();
            large_prepare = tx->prepare_stats();
        }
    }

    // The drag reaches the same two balls either way...
    CHECK(small_prepared == 2);
    CHECK(large_prepared == 2);
    // ...so a frame visits exactly the two items it moves, whatever else the
    // layer holds. A counter rather than a clock: this must hold on a busy CI
    // machine as firmly as on an idle one.
    CHECK(small_visits == 2);
    CHECK(large_visits == 2);
    // And the traversal that DOES scale happened once, at begin.
    CHECK(small_prepare.visited == 2);
    CHECK(large_prepare.visited == 5002);
    CHECK(large_prepare.reached == 2);
}

// -- 12.11: one gesture, one undo step ---------------------------------------

TEST_CASE("sculpt: a whole Move drag is one undo step across every affected item") {
    Document doc = two_balls();
    UndoStack undo;
    const std::vector<std::uint8_t> before = snapshot(doc);

    auto tx = SdfMoveTransaction::begin(doc, doc.layers.front().id, cf3(0, 0, 0),
                                        brush::MoveSettings{0.8f, 0, false});
    REQUIRE(tx);
    for (int i = 1; i <= 100; ++i) tx->update(cf3(0, 0.004f * static_cast<float>(i), 0));
    REQUIRE(tx->commit(&undo));

    CHECK(undo.undo_depth() == 1);
    const std::vector<std::uint8_t> committed = snapshot(doc);
    for (NodeId n : doc.layers.front().sdf->roots) {
        const Node* node = doc.layers.front().sdf->find(n);
        CHECK(node->deformers.size() == 1);  // one warp per item, not a hundred
        CHECK(node->deformers.front().type == kernel::cdeform_grab);
    }

    REQUIRE(undo.undo(doc));
    CHECK(snapshot(doc) == before);
    REQUIRE(undo.redo(doc));
    CHECK(snapshot(doc) == committed);
}

TEST_CASE("sculpt: a Move commit refuses a layer that changed underneath it") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    UndoStack undo;

    auto tx = SdfMoveTransaction::begin(doc, id, cf3(0, 0, 0), brush::MoveSettings{0.8f, 0, false});
    REQUIRE(tx);
    tx->update(cf3(0, 0.3f, 0));

    Node extra;
    extra.prim = Prim::sphere(0.2f);
    REQUIRE(undo.perform(doc, Command{AddNodeCmd{id, kNoNode, -1, {extra}}}));
    const std::vector<std::uint8_t> external = snapshot(doc);
    const std::size_t depth = undo.undo_depth();

    CHECK_FALSE(tx->commit(&undo));
    CHECK(snapshot(doc) == external);
    CHECK(undo.undo_depth() == depth);
}

TEST_CASE("sculpt: a Move transaction refuses what is not a drag") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    CHECK_FALSE(SdfMoveTransaction::begin(doc, id, cf3(0, 0, 0), brush::MoveSettings{0.0f}));
    CHECK_FALSE(SdfMoveTransaction::begin(doc, 9999, cf3(0, 0, 0), brush::MoveSettings{0.8f}));

    // A drag over empty space is not an error: it is a valid gesture that moves
    // nothing, and it must commit without inventing a step.
    UndoStack undo;
    auto far = SdfMoveTransaction::begin(doc, id, cf3(50, 0, 0), brush::MoveSettings{0.2f});
    REQUIRE(far);
    CHECK(far->affected_count() == 0);
    far->update(cf3(0, 0.2f, 0));
    CHECK(far->commit(&undo));
    CHECK(undo.undo_depth() == 0);
}

TEST_CASE("sculpt: a cancelled Move leaves the document exactly as it was") {
    Document doc = two_balls();
    const std::vector<std::uint8_t> before = snapshot(doc);
    auto tx = SdfMoveTransaction::begin(doc, doc.layers.front().id, cf3(0, 0, 0),
                                        brush::MoveSettings{0.8f, 0, false});
    REQUIRE(tx);
    for (int i = 0; i < 30; ++i) tx->update(cf3(0, 0.01f * static_cast<float>(i), 0));
    tx->cancel();
    CHECK(snapshot(doc) == before);
    CHECK_FALSE(tx->live());
    CHECK_FALSE(tx->commit(nullptr));
}

// -- 12.12 / 12.13 / 12.14: the complexity policy ----------------------------

namespace {

// One completed Move stroke, start to finish, exactly as a host would drive it.
bool move_stroke(Document& doc, LayerId id, const SdfSculptPolicy& policy, cfloat3 anchor,
                 cfloat3 total, UndoStack* undo) {
    auto tx = SdfMoveTransaction::begin(doc, id, anchor, brush::MoveSettings{0.8f, 0, false},
                                        policy);
    if (!tx) return false;
    for (int i = 1; i <= 4; ++i)
        tx->update(total * (static_cast<float>(i) / 4.0f));
    return tx->commit(undo);
}

}  // namespace

TEST_CASE("sculpt: separate Move strokes still compose, and one drag still does not") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    SdfSculptPolicy policy = smooth_policy();  // consolidation left off

    // Distinct anchors, so no stroke's leading grab is mistaken for another's
    // continuation — that replacement is for FRAMES of one drag, not strokes.
    for (int s = 0; s < 3; ++s) {
        const float y = 0.05f * static_cast<float>(s);
        REQUIRE(move_stroke(doc, id, policy, cf3(0, y, 0), cf3(0, 0.1f, 0), nullptr));
    }
    const FieldReport report = report_layer(doc.layers.front());
    CHECK(report.longest_deformer_chain == 3);  // three strokes, three grabs

    // ...and each of those strokes was four frames long.
    CHECK(report.safe_step_scale < 1.0f);
}

TEST_CASE("sculpt: an unauthorised policy reports over-budget and bakes nothing") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    SdfSculptPolicy policy = smooth_policy();
    policy.complexity.max_deformer_chain = 1;  // crossed by the second stroke
    policy.complexity.allow_consolidation = false;

    REQUIRE(move_stroke(doc, id, policy, cf3(0, 0.0f, 0), cf3(0, 0.1f, 0), nullptr));
    auto tx = SdfMoveTransaction::begin(doc, id, cf3(0, 0.05f, 0),
                                        brush::MoveSettings{0.8f, 0, false}, policy);
    REQUIRE(tx);
    tx->update(cf3(0, 0.1f, 0));
    REQUIRE(tx->commit(nullptr));

    CHECK(tx->budget().over_budget);
    CHECK_FALSE(tx->budget().consolidated);
    // Everything the artist authored is still there and still parametric.
    CHECK(doc.layers.front().sdf->roots.size() == 2);
    CHECK_FALSE(consolidation_state(doc.layers.front()));
    for (NodeId n : doc.layers.front().sdf->roots)
        CHECK(doc.layers.front().sdf->find(n)->prim.type == PrimType::Sphere);
}

TEST_CASE("sculpt: an authorised policy collapses the layer inside the stroke's own undo") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    UndoStack undo;
    SdfSculptPolicy policy = smooth_policy();
    policy.complexity.max_deformer_chain = 1;
    policy.complexity.allow_consolidation = true;

    REQUIRE(move_stroke(doc, id, policy, cf3(0, 0.0f, 0), cf3(0, 0.1f, 0), &undo));
    CHECK(undo.undo_depth() == 1);
    const std::vector<std::uint8_t> after_first = snapshot(doc);

    auto tx = SdfMoveTransaction::begin(doc, id, cf3(0, 0.05f, 0),
                                        brush::MoveSettings{0.8f, 0, false}, policy);
    REQUIRE(tx);
    tx->update(cf3(0, 0.1f, 0));
    REQUIRE(tx->commit(&undo));

    CHECK(tx->budget().over_budget);
    CHECK(tx->budget().consolidated);
    CHECK(consolidation_state(doc.layers.front()));
    // The chain degradation is gone: a volume carries no deformers.
    const FieldReport after = report_layer(doc.layers.front());
    CHECK(after.longest_deformer_chain == 0);
    CHECK(after.safe_step_scale > 0.0f);

    // ONE step for the stroke AND its consolidation — 6.8. Without nested
    // grouping this would be two, and undoing the drag would leave the artist
    // looking at a baked layer.
    CHECK(undo.undo_depth() == 2);
    REQUIRE(undo.undo(doc));
    CHECK(snapshot(doc) == after_first);
}

TEST_CASE("sculpt: a Smooth commit does not re-bake a layer it just sampled") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    SdfSculptPolicy policy = smooth_policy();
    // A budget nothing can satisfy, so the only thing stopping a second bake is
    // the check that the layer is already consolidated.
    policy.complexity.min_safe_step_scale = 1.5f;
    policy.complexity.allow_consolidation = true;

    auto tx = SdfSmoothTransaction::begin(doc, id, policy);
    REQUIRE(tx);
    tx->update(dab(cf3(0, 0.42f, 0)));
    const std::vector<std::uint8_t> preview = tx->preview_volume().serialize();
    REQUIRE(tx->commit(nullptr));

    CHECK(tx->budget().over_budget);
    CHECK_FALSE(tx->budget().consolidated);  // 6.6: already sampled, nothing to collapse
    const Layer& after = doc.layers.front();
    REQUIRE(after.sdf->roots.size() == 1);
    // Byte-identical to what the stroke previewed: a re-bake would have
    // produced a volume of a volume, at a different Lipschitz.
    CHECK(after.sdf->find(after.sdf->roots.front())->volume->serialize() == preview);
}

TEST_CASE("sculpt: an empty policy authorises nothing and is never over budget") {
    Document doc = two_balls();
    const FieldReport report = report_layer(doc.layers.front());
    CHECK_FALSE(session::over_sculpt_budget(session::SdfSculptComplexityPolicy{}, report));

    session::SdfSculptComplexityPolicy items;
    items.max_item_count = 1;
    CHECK(session::over_sculpt_budget(items, report));  // the layer holds two
}

// -- the source stamp itself --------------------------------------------------

TEST_CASE("sculpt: a layer fingerprint moves for any edit that changes the field") {
    Document doc = two_balls();
    Layer& layer = doc.layers.front();
    const std::uint64_t base = session::layer_fingerprint(layer);
    CHECK(session::layer_fingerprint(layer) == base);  // stable when nothing moves

    Layer moved = layer;
    moved.xform.position = cf3(0.1f, 0, 0);
    CHECK(session::layer_fingerprint(moved) != base);

    Layer edited = layer;
    edited.sdf = std::make_shared<SdfContent>(*layer.sdf);
    edited.sdf->find_mut(edited.sdf->roots.front())->prim = Prim::sphere(0.6f);
    CHECK(session::layer_fingerprint(edited) != base);

    Layer warped = layer;
    warped.sdf = std::make_shared<SdfContent>(*layer.sdf);
    warped.sdf->find_mut(warped.sdf->roots.front())->deformers.push_back(
        Deformer::grab(cf3(0, 0, 0), 0.5f, cf3(0, 0.1f, 0)));
    CHECK(session::layer_fingerprint(warped) != base);

    // An INSTANCE shares its content, so an edit through a sibling is an edit
    // here — the digest must read the CONTENT and not the pointer, which has
    // not moved.
    Layer instanced = layer;
    CHECK(session::layer_fingerprint(instanced) == base);
    Node fresh;
    fresh.prim = Prim::sphere(0.1f);
    instanced.sdf->insert(fresh);
    CHECK(session::layer_fingerprint(instanced) != base);
    CHECK(session::layer_fingerprint(layer) != base);  // the sibling sees it too
}

TEST_CASE("sculpt: a fingerprint is a function of the VALUES, not of the bytes around them") {
    // Regression for the shape of hash this cannot be. Mixing a struct through
    // memcpy folds its tail PADDING in — Transition, Repeat and Profile all
    // have some — and padding is not a value: two layers equal in every field
    // would hash apart because one was built by a copy that left different
    // bytes in the holes, and a transaction would refuse to commit against a
    // document nobody had touched.
    auto build = [](float radius) {
        Document doc;
        Layer& l = doc.add_sdf_layer("body");
        Node n;
        n.prim = Prim::sphere(radius);
        n.transition.ease = 3;
        n.repeat = Repeat::radial(6, 0.2f);
        n.profile = Profile::trapezoid(0.4f, 0.2f, 0.3f);
        n.profiles.push_back(Profile::hexagon(0.25f));
        n.deformers.push_back(Deformer::grab(cf3(0, 0, 0), 0.5f, cf3(0, 0.1f, 0)));
        l.sdf->insert(n);
        return doc;
    };

    // Two documents built the same way, in separately allocated storage, must
    // agree; one built differently must not.
    Document a = build(0.5f);
    Document b = build(0.5f);
    Document c = build(0.6f);
    const std::uint64_t base = session::layer_fingerprint(a.layers.front());
    CHECK(session::layer_fingerprint(b.layers.front()) == base);
    CHECK(session::layer_fingerprint(c.layers.front()) != base);

    // ...and so must a plain value copy, which is what an undo record holds.
    const Layer copied = a.layers.front();
    CHECK(session::layer_fingerprint(copied) == base);

    // Each of the padded aggregates on its own, so a future field added to one
    // of them cannot quietly stop being read.
    for (int i = 0; i < 3; ++i) {
        Document d = build(0.5f);
        Node* n = d.layers.front().sdf->find_mut(d.layers.front().sdf->roots.front());
        if (i == 0) n->transition.r1 = 2.0f;
        if (i == 1) n->repeat.spacing = cf3(2, 2, 2);
        if (i == 2) n->profile.params[1] = 0.9f;
        CAPTURE(i);
        CHECK(session::layer_fingerprint(d.layers.front()) != base);
    }
}

TEST_CASE("sculpt: an edit to ANOTHER layer does not stale a transaction") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    Layer& other = doc.add_sdf_layer("elsewhere");
    const LayerId other_id = other.id;
    UndoStack undo;

    auto tx = SdfSmoothTransaction::begin(doc, id, smooth_policy());
    REQUIRE(tx);
    tx->update(dab(cf3(0, 0.42f, 0)));

    Node extra;
    extra.prim = Prim::sphere(0.3f);
    REQUIRE(undo.perform(doc, Command{AddNodeCmd{other_id, kNoNode, -1, {extra}}}));

    // The transaction is about ONE layer, so only that layer's state can
    // invalidate it. Refusing here would make a two-window host unusable.
    CHECK(tx->commit(&undo));
}

TEST_CASE("sculpt: a Move that applied nothing does not trigger a policy bake") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    UndoStack undo;
    SdfSculptPolicy policy = smooth_policy();
    policy.complexity.max_item_count = 1;  // the layer holds two: always over budget
    policy.complexity.allow_consolidation = true;

    // A drag with no displacement issues no command, so it is not a completed
    // stroke — and a bake it triggered would be a destructive edit the artist
    // never made.
    auto still = SdfMoveTransaction::begin(doc, id, cf3(0, 0, 0),
                                           brush::MoveSettings{0.8f, 0, false}, policy);
    REQUIRE(still);
    still->update(cf3(0, 0, 0));
    REQUIRE(still->commit(&undo));
    CHECK(still->budget().over_budget);       // still reported...
    CHECK_FALSE(still->budget().consolidated);  // ...and still not acted on
    CHECK(undo.undo_depth() == 0);
    CHECK(doc.layers.front().sdf->roots.size() == 2);

    // A drag that does move something crosses the same budget and IS acted on,
    // so the case above is not passing because the policy never fires.
    auto moved = SdfMoveTransaction::begin(doc, id, cf3(0, 0, 0),
                                           brush::MoveSettings{0.8f, 0, false}, policy);
    REQUIRE(moved);
    moved->update(cf3(0, 0.2f, 0));
    REQUIRE(moved->commit(&undo));
    CHECK(moved->budget().consolidated);
    CHECK(consolidation_state(doc.layers.front()));
}
