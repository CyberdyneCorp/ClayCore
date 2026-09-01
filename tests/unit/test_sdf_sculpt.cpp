// Transient SDF sculpt transactions (sdf-sculpt-transaction spec): a live
// Smooth that changes its preview before pointer-up, a live Move that never
// writes to the document while the pointer is down, and a session policy that
// keeps repeated sculpting bounded without the engine deciding to bake.
//
// The first thing every test here asserts is a NEGATIVE: that the persistent
// document did not change. That is the whole architecture, and a preview that
// looks right while quietly mutating the document is the defect this replaces.

#include <doctest/doctest.h>

#include <algorithm>
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

// The layer sampled onto `like`'s lattice with NO redistance: what the lazy
// working field materializes, and therefore what a reference for it must be.
field::FieldVolume raw_sample(const Layer& layer, const field::FieldVolume& like) {
    Layer view = layer;
    view.visible = true;
    view.xform = math::Transform{};
    const Tape tape = compile_layer(view);
    const math::Aabb region{like.origin(),
                            like.origin() + cf3(float(like.sample_extent(0) - 1),
                                                float(like.sample_extent(1) - 1),
                                                float(like.sample_extent(2) - 1)) *
                                                like.cell_size()};
    return field::FieldVolume::sample_blocks(
        [&tape](const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
                float* out) {
            for (std::size_t s = 0; s < count; ++s)
                for (int i = 0; i < field::kBrickSamples; ++i) {
                    const cfloat3 p = grid.sample_position(first + s, i);
                    out[s * field::kBrickSamples + i] = tape.eval(p).d;
                }
        },
        region, like.cell_size(), like.band());
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

TEST_CASE("sculpt: the lazy working field equals a whole-layer relax sequence") {
    // The working field is the layer SAMPLED and then relaxed, materialized
    // around the brush instead of everywhere. So the reference is a whole-layer
    // sample relaxed by the same dabs -- not `bake_layer`, which redistances,
    // and whose post-process the lazy path applies once at commit instead.
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;

    const field::RelaxSettings a = dab(cf3(0, 0.42f, 0), 0.30f, 0.7f);
    const field::RelaxSettings b = dab(cf3(0.2f, 0.40f, 0), 0.25f, 0.5f);
    const field::RelaxSettings c = dab(cf3(-0.2f, 0.40f, 0), 0.20f, 0.9f);

    auto tx = SdfSmoothTransaction::begin(doc, id, smooth_policy());
    REQUIRE(tx);
    tx->update(a);
    tx->update(b);
    tx->update(c);

    // The same lattice, sampled whole, relaxed the same way.
    field::FieldVolume expected = raw_sample(doc.layers.front(), tx->preview_volume());
    field::relax_in_place(expected, a);
    field::relax_in_place(expected, b);
    field::relax_in_place(expected, c);

    // Compared only where the lazy field HAS materialized: elsewhere it holds
    // nothing by design, which is the whole point.
    // Split by whether the sample is IN THE BAND, because that is where the
    // field is a distance and where anyone looks. Past the band both fields
    // hold a bound rather than a distance, and they hold different ones on
    // purpose: materialization force-stores every brick it fills, so relax
    // writes bricks a normal bake would have left empty.
    double worst_band = 0.0, worst_far = 0.0;
    std::size_t in_band = 0, far = 0;
    const field::FieldVolume& got = tx->preview_volume();
    const float band = expected.band();
    for (int gz = 0; gz < expected.sample_extent(2); ++gz)
        for (int gy = 0; gy < expected.sample_extent(1); ++gy)
            for (int gx = 0; gx < expected.sample_extent(0); ++gx) {
                const std::optional<float> mine = got.sample_at(gx, gy, gz);
                const std::optional<float> want = expected.sample_at(gx, gy, gz);
                if (!mine || !want) continue;
                const double d = std::abs(double(*mine) - double(*want));
                if (std::abs(*want) <= band) {
                    worst_band = std::max(worst_band, d);
                    ++in_band;
                } else {
                    worst_far = std::max(worst_far, d);
                    ++far;
                }
            }
    MESSAGE("in band: " << in_band << " worst " << worst_band
            << " | past band: " << far << " worst " << worst_far);
    CHECK(in_band > 1000);
    // 5.5e-5 measured, about a thousandth of a cell. Not float rounding, and
    // worth naming: at the BAND EDGE a written sample's stencil reaches bricks
    // that the lazy field force-stored and a normal bake left empty, so the tap
    // exists here and is missing there, and relax renormalizes over a different
    // neighbourhood. It is the price of using stored-ness as the record of what
    // has been materialized, and it is three orders below a cell.
    CHECK(worst_band < 1e-3);
    // Past the band both hold a bound rather than a distance, and deliberately
    // different ones. Bounded so a real divergence still fails.
    CHECK(worst_far < 0.2 * kCell);
}

TEST_CASE("sculpt: how far the lazy commit sits from the whole-layer path") {
    // The one semantic difference, MEASURED rather than asserted away. The old
    // path relaxed a redistanced bake; this redistances a relaxed field. Both
    // are sound distance fields and neither approximates the other, so the
    // useful question is how far apart they land on the surface an artist sees.
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    const field::RelaxSettings a = dab(cf3(0, 0.42f, 0), 0.30f, 0.7f);

    // The old path, reproduced exactly: bake (redistanced), then relax.
    ConsolidationParams params;
    params.cell_size = kCell;
    field::FieldVolume old_path = *bake_layer(doc.layers.front(), params);
    field::relax_in_place(old_path, a);

    auto tx = SdfSmoothTransaction::begin(doc, id, smooth_policy());
    REQUIRE(tx);
    tx->update(a);
    REQUIRE(tx->commit(nullptr));
    const Layer& after = doc.layers.front();
    REQUIRE(after.sdf->roots.size() == 1);
    const field::FieldVolume& new_path = *after.sdf->find(after.sdf->roots.front())->volume;

    // On the surface, which is what the two disagree about in a way anyone can
    // see. Marched along +y through the seam the dab smoothed.
    double worst = 0.0;
    for (float x = -0.6f; x <= 0.6f; x += 0.02f)
        for (float y = 0.0f; y <= 0.8f; y += 0.02f) {
            const cfloat3 p = cf3(x, y, 0);
            if (!old_path.has_samples_at(p) || !new_path.has_samples_at(p)) continue;
            worst = std::max(worst,
                             std::abs(double(old_path.eval(p)) - double(new_path.eval(p))));
        }
    MESSAGE("lazy commit vs whole-layer commit, worst on surface: " << worst
            << " (" << (worst / kCell) << " cells)");
    // Recorded as a bound rather than a target: what matters is that it is a
    // fraction of a cell and not a feature.
    CHECK(worst < 1.0 * kCell);
}

// -- 12.6: the source layer is evaluated once, at begin, and never again ------

TEST_CASE("sculpt: begin materializes nothing, and dabs materialize locally") {
    // THE DEFINING TEST of the lazy path. `begin` used to bake the whole layer;
    // it now evaluates nothing at all, and the first dab brings in only the
    // bricks it will read. Counters rather than a clock, because the claim is
    // about what is touched and has to hold on a loaded machine.
    Document doc = two_balls();
    UndoStack undo;
    auto tx = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(tx);

    CHECK(tx->materialization().materialized_bricks == 0);
    CHECK(tx->materialization().updates == 0);
    CHECK(tx->preview_volume().brick_count() == 0);  // a lattice, and no samples

    const cfloat3 seam = cf3(0, 0.42f, 0);
    tx->update(dab(seam, 0.25f));
    const std::size_t first = tx->materialization().materialized_bricks;
    CHECK(first > 0);
    // No ratio against this fixture's own size: two balls at a 0.05 cell are
    // about 96 stored bricks, and any correct halo is dominated by brick
    // granularity when the brush is five cells and a brick is eight. What
    // "local" means is that the count does not follow the MODEL, and the
    // scaling test below is where that is asserted.

    // The SAME dab again brings in nothing new and reuses what is there.
    tx->update(dab(seam, 0.25f));
    CHECK(tx->materialization().materialized_bricks == first);
    CHECK(tx->materialization().reused_bricks > 0);

    // A dab somewhere else materializes its own region and no more.
    tx->update(dab(cf3(0, -0.42f, 0), 0.25f));
    const std::size_t after_far = tx->materialization().materialized_bricks;
    CHECK(after_far > first);
    CHECK(after_far - first <= first + 2);  // its own ball, not the model

    REQUIRE(tx->commit(&undo));
    CHECK(undo.undo_depth() == 1);
}

TEST_CASE("sculpt: distant unrelated model does not change what a dab materializes") {
    // 3.4 / 10.2: after the brush region is fixed, adding geometry far away
    // must not change the local working set. The number is a count, so this
    // holds whatever the machine is doing.
    std::size_t small = 0, large = 0;
    for (std::size_t extra : {std::size_t{0}, std::size_t{400}}) {
        Document doc = two_balls_plus_distant(extra);
        auto tx = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
        REQUIRE(tx);
        tx->update(dab(cf3(0, 0.42f, 0), 0.25f));
        (extra == 0 ? small : large) = tx->materialization().materialized_bricks;
    }
    CHECK(small > 0);
    CHECK(small == large);
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
    // Every materialized sample is the source's, verbatim. Against the RAW
    // source rather than a bake: the working field is the layer sampled, and a
    // bake's redistance is applied once at commit instead.
    const field::FieldVolume raw = raw_sample(doc.layers.front(), tx->preview_volume());
    std::size_t checked = 0;
    for (int gz = 0; gz < raw.sample_extent(2); ++gz)
        for (int gy = 0; gy < raw.sample_extent(1); ++gy)
            for (int gx = 0; gx < raw.sample_extent(0); ++gx) {
                const std::optional<float> mine = tx->preview_volume().sample_at(gx, gy, gz);
                const std::optional<float> want = raw.sample_at(gx, gy, gz);
                if (!mine || !want) continue;
                REQUIRE(*mine == *want);  // frozen means verbatim, not nearly
                ++checked;
            }
    CHECK(checked > 1000);

    // Half a mask scales the effect rather than gating it.
    field::RelaxSettings half = dab(frozen, 0.35f);
    half.mask = [](cfloat3) { return 0.5f; };
    auto partial = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(partial);
    // Half a mask scales the effect rather than gating it: something moved,
    // and it moved less than an unmasked dab would have.
    CHECK(partial->update(half).changed);
    auto unmasked = SdfSmoothTransaction::begin(doc, doc.layers.front().id, smooth_policy());
    REQUIRE(unmasked);
    unmasked->update(dab(frozen, 0.35f));
    const cfloat3 probe = cf3(0, 0.42f, 0);
    const double base = raw.eval(probe);
    const double half_moved = std::abs(double(partial->preview_volume().eval(probe)) - base);
    const double full_moved = std::abs(double(unmasked->preview_volume().eval(probe)) - base);
    CHECK(half_moved < full_moved);
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

// -- a live drag under a layer mirror (#363) ----------------------------------

TEST_CASE("sculpt: a live Move under a mirror is the drag move_brush commits") {
    // The prepared drag carries every image the layer's symmetry makes of the
    // ball, so a live drag near the plane previews and commits what the
    // one-step resolver produces: the item under the ball moves, the item whose
    // COPY sits under the ball moves through that copy, and a straddler takes
    // one grab per image. Sample for sample, because the two are one resolver.
    const auto make = [] {
        Document doc;
        Layer& layer = doc.add_sdf_layer("mirrored");
        layer.mirror_axes = kMirrorX;
        layer.mirror_k = 0.05f;
        const auto ball = [&](cfloat3 pos, float r) {
            Node n;
            n.prim = Prim::sphere(r);
            n.op = Op::Add;
            n.blend = Blend{BlendProfile::Quadratic, 0.05f};
            n.xform.position = pos;
            return layer.sdf->insert(n);
        };
        struct Ids {
            Document doc;
            NodeId base, A, B;
        } r{std::move(doc), kNoNode, kNoNode, kNoNode};
        r.base = ball(cf3(0, 0, 0), 0.4f);       // straddles the plane
        r.A = ball(cf3(1.0f, 0.3f, 0), 0.2f);    // under the ball
        r.B = ball(cf3(-1.0f, -0.3f, 0), 0.2f);  // its copy is under the ball
        return r;
    };
    const brush::MoveSettings settings{0.45f, 0, false};
    const cfloat3 centre = cf3(1.1f, 0, 0), total = cf3(0.2f, 0, 0);

    auto live = make();
    const std::vector<std::uint8_t> before = snapshot(live.doc);
    auto tx = SdfMoveTransaction::begin(live.doc, live.doc.layers.front().id, centre, settings);
    REQUIRE(tx);
    // A and B, through the ball and its reflection; the base is out of reach of
    // both (the mirror-expanded bound used to take it, for a warp that did
    // nothing).
    REQUIRE(tx->affected_count() == 2);
    for (int i = 1; i <= 20; ++i) tx->update(total * (static_cast<float>(i) / 20.0f));
    CHECK(snapshot(live.doc) == before);

    // The preview carries one grab on each, B's at the REFLECTED centre.
    std::vector<Deformer> grabs;
    REQUIRE(tx->preview_grabs(live.B, &grabs));
    REQUIRE(grabs.size() == 1);
    CHECK(grabs[0].k == doctest::Approx(-0.1f));
    CHECK(grabs[0].ext[0] == doctest::Approx(-0.2f));
    CHECK_FALSE(tx->preview_grabs(live.base, &grabs));

    // The one-step resolver on a fresh document, committed by hand.
    auto once = make();
    const std::vector<brush::MoveWarp> warps =
        brush::move_brush(once.doc.layers.front(), centre, total, settings);
    REQUIRE(warps.size() == 2);
    for (const brush::MoveWarp& w : warps) {
        const Node* n = once.doc.layers.front().sdf->find(w.node);
        REQUIRE(scene::apply(once.doc, Command{SetDeformersCmd{once.doc.layers.front().id, w.node,
                                                               brush::moved_chain(*n, w)}}));
    }
    const auto same_field = [](const Layer& a, const Layer& b) {
        const Tape ta = compile_layer(a), tb = compile_layer(b);
        for (float z = -0.5f; z <= 0.5f; z += 0.1f)
            for (float y = -0.7f; y <= 0.7f; y += 0.05f)
                for (float x = -1.6f; x <= 1.6f; x += 0.05f)
                    CHECK(ta.eval(cf3(x, y, z)).d == tb.eval(cf3(x, y, z)).d);
    };
    same_field(tx->preview_layer(), once.doc.layers.front());

    UndoStack undo;
    REQUIRE(tx->commit(&undo));
    CHECK(undo.undo_depth() == 1);
    same_field(live.doc.layers.front(), once.doc.layers.front());

    // The material under the ball moved whether it is an item or a copy, and
    // by the same amount.
    const auto untouched = make();
    const Tape was = compile_layer(untouched.doc.layers.front());
    const Tape now = compile_layer(live.doc.layers.front());
    const float moved_a = now.eval(cf3(1.2f, 0.3f, 0)).d - was.eval(cf3(1.2f, 0.3f, 0)).d;
    const float moved_b = now.eval(cf3(1.2f, -0.3f, 0)).d - was.eval(cf3(1.2f, -0.3f, 0)).d;
    CHECK(moved_a < -0.05f);
    CHECK(moved_b == moved_a);
}

TEST_CASE("sculpt: a live Move on the plane gives a straddler both grabs, every frame") {
    Document doc;
    Layer& layer = doc.add_sdf_layer("mirrored");
    layer.mirror_axes = kMirrorX;
    Node n;
    n.prim = Prim::sphere(0.4f);
    const NodeId base = layer.sdf->insert(n);
    const brush::MoveSettings settings{0.5f, 0, false};

    auto tx = SdfMoveTransaction::begin(doc, layer.id, cf3(0, 0, 0), settings);
    REQUIRE(tx);
    REQUIRE(tx->affected_count() == 1);
    for (int i = 1; i <= 5; ++i) {
        tx->update(cf3(0.05f * static_cast<float>(i), 0.02f, 0));
        // One grab per image, replaced frame to frame rather than stacked.
        CHECK(tx->preview_layer().sdf->find(base)->deformers.size() == 2);
    }
    std::vector<Deformer> grabs;
    REQUIRE(tx->preview_grabs(base, &grabs));
    REQUIRE(grabs.size() == 2);
    CHECK(grabs[0].ext[0] == -grabs[1].ext[0]);  // opposite pulls across the plane
    CHECK(grabs[0].ext[1] == grabs[1].ext[1]);   // the same pull along it

    UndoStack undo;
    REQUIRE(tx->commit(&undo));
    CHECK(doc.layers.front().sdf->find(base)->deformers.size() == 2);
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
    REQUIRE(tx->commit(nullptr));

    CHECK(tx->budget().over_budget);
    CHECK_FALSE(tx->budget().consolidated);  // 6.6: already sampled, nothing to collapse
    const Layer& after = doc.layers.front();
    REQUIRE(after.sdf->roots.size() == 1);
    // One volume item carrying no deformers: the state a bake produces, so
    // there is nothing left for the policy to collapse.
    CHECK(report_layer(after).longest_deformer_chain == 0);
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

TEST_CASE("sculpt: an authorised policy keeps firing, not just once") {
    // REGRESSION. The post-stroke check used to skip on consolidation_state()
    // alone — "the layer is already one volume item, nothing to collapse".
    // Consolidating MAKES that predicate true forever after, so the policy
    // fired on the stroke that first crossed the budget and never again, while
    // every later drag stacked another grab on the volume item and the safe
    // step decayed exactly as it had before. Measured on the benchmark: a
    // hundred drags ended at a chain of 58 with ONE consolidation.
    //
    // A volume item carrying a deformer chain is not "already collapsed": a
    // bake absorbs those grabs into the samples. Only a volume with an EMPTY
    // chain is, which is what Smooth's own commit leaves behind.
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    SdfSculptPolicy policy = smooth_policy();
    policy.complexity.max_deformer_chain = 2;  // every third stroke crosses it
    policy.complexity.allow_consolidation = true;

    int consolidations = 0;
    int longest_seen = 0;
    for (int stroke = 0; stroke < 12; ++stroke) {
        const float y = 0.01f * static_cast<float>(stroke);
        auto tx = SdfMoveTransaction::begin(doc, id, cf3(0, y, 0),
                                            brush::MoveSettings{0.8f, 0, false}, policy);
        REQUIRE(tx);
        tx->update(cf3(0, 0.05f, 0));
        REQUIRE(tx->commit(nullptr));
        if (tx->budget().consolidated) ++consolidations;
        longest_seen = std::max(longest_seen, tx->budget().report.longest_deformer_chain);
    }

    // Fired more than once — the whole point — and the chain never ran away.
    CHECK(consolidations > 1);
    CHECK(longest_seen <= policy.complexity.max_deformer_chain + 1);
    CHECK(report_layer(doc.layers.front()).longest_deformer_chain <=
          policy.complexity.max_deformer_chain + 1);
}

// What a commit costs the marcher, and the ceiling that puts under the policy.
//
// The layer that comes back from a commit is one SAMPLED VOLUME, and
// kernel::cfi_volume declares sqrt(3) * max(sample_lipschitz, 1) for one — so
// 1/sqrt(3) = 0.577 is the BEST a consolidated layer can ever report, however
// clean its samples measure. That is worth a number rather than a shrug,
// because it decides two things a host gets wrong otherwise: a fixed step
// budget draws the committed layer worse than the parametric one it replaced
// (issue #379), and a min_safe_step_scale above 0.577 is unsatisfiable, so the
// layer stays over budget for ever while the collapse is correctly refused.
//
// The case below uses 1.5 for "nothing can satisfy this". This one pins where
// "nothing" actually starts.
TEST_CASE("sculpt: a commit lands at the ceiling a sampled volume declares") {
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    const float before = compile_document(doc).safe_step_scale();
    CHECK(before == doctest::Approx(1.0f));  // spheres and a smooth union

    SdfSculptPolicy policy = smooth_policy();
    // Just above the ceiling, not far above it: the point is that 0.6 is
    // already out of reach, not that 1.5 is.
    policy.complexity.min_safe_step_scale = 0.6f;
    policy.complexity.allow_consolidation = true;

    auto tx = SdfSmoothTransaction::begin(doc, id, policy);
    REQUIRE(tx);
    tx->update(dab(cf3(0, 0.42f, 0)));
    REQUIRE(tx->commit(nullptr));

    const float after = compile_document(doc).safe_step_scale();
    CAPTURE(after);
    CHECK(after == doctest::Approx(1.0f / std::sqrt(3.0f)).epsilon(1e-4));
    CHECK(tx->budget().report.safe_step_scale == doctest::Approx(after));

    // Over budget, and nothing to do about it: re-baking a layer that is
    // already one volume would only make it steeper.
    CHECK(tx->budget().over_budget);
    CHECK_FALSE(tx->budget().consolidated);
    REQUIRE(doc.layers.front().sdf->roots.size() == 1);
}

TEST_CASE("sculpt: a bare consolidated layer is still not re-baked") {
    // The other half of the pair above: relaxing the check must not turn it
    // off. A single volume item with an EMPTY chain is what a bake produces,
    // and baking it again resamples samples into samples — measurably steeper,
    // which is the degradation consolidate.h opens by describing.
    Document doc = two_balls();
    const LayerId id = doc.layers.front().id;
    SdfSculptPolicy policy = smooth_policy();
    policy.complexity.min_safe_step_scale = 1.5f;  // nothing can satisfy this
    policy.complexity.allow_consolidation = true;

    auto tx = SdfSmoothTransaction::begin(doc, id, policy);
    REQUIRE(tx);
    tx->update(dab(cf3(0, 0.42f, 0)));
    REQUIRE(tx->commit(nullptr));
    CHECK(tx->budget().over_budget);
    CHECK_FALSE(tx->budget().consolidated);
    CHECK(report_layer(doc.layers.front()).longest_deformer_chain == 0);
    const std::vector<std::uint8_t> first_bytes = snapshot(doc);

    // A second Smooth over the already-baked layer: still nothing to collapse,
    // and still byte-identical to what it previewed.
    auto again = SdfSmoothTransaction::begin(doc, id, policy);
    REQUIRE(again);
    again->update(dab(cf3(0, 0.42f, 0)));
    REQUIRE(again->commit(nullptr));
    CHECK_FALSE(again->budget().consolidated);
    const Layer& after = doc.layers.front();
    REQUIRE(after.sdf->roots.size() == 1);
    CHECK(report_layer(after).longest_deformer_chain == 0);
    CHECK(snapshot(doc) != first_bytes);  // the second stroke did something
}
