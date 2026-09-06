// THE GATES A LAYER BOOLEAN HAS TO PASS
// (fold-the-layers-with-an-operator, tasks 6.1-6.6).
//
// The fold itself is test_layer_fold.cpp, the two-forms-agree parity is
// test_layer_parity.cpp, and the sites that held two halves apart are
// test_layer_fold_sites.cpp. This file is the acceptance list: the claims the
// change is FOR, each stated the way a host would notice it failing.
//
// TWO OF THEM HAVE AN INVALIDATION HALF THAT THE GEOMETRY CANNOT SEE, and that
// is why they are here rather than folded into the fold tests. Hiding a layer
// and reordering the stack both change the field, and both go through a dirty
// REGION -- a box outside which the brick cache keeps what it already had and
// re-stamps it to the new revision (the `rev == now` copy in `resume_bricks`).
// Compiling the document afresh after either edit therefore proves nothing
// about them: the compile does not consult a seed. A subtracting layer's box
// covers what it changed, so the naive region is right for it and every such
// gate passes; an INTERSECTING layer takes material away everywhere the layers
// beneath have any, so its box has to be the accumulated extent BENEATH it, and
// with the naive region the bricks outside its own box keep a stale answer with
// nothing to report it. Both cases below go through `clay_brick_cache_eval_-
// requests` against a document built the same way from scratch, and both put
// their bricks where only the wide region reaches.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "clay.h"
#include "clay/field/volume.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/to_field.h"
#include "clay/scene/commands.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// -- sampling ----------------------------------------------------------------

std::vector<cfloat3> lattice(int n, float half) {
    std::vector<cfloat3> pts;
    pts.reserve(static_cast<std::size_t>(n) * n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                auto a = [n, half](int v) {
                    return -half + 2.0f * half * static_cast<float>(v) / (n - 1);
                };
                pts.push_back(cf3(a(i), a(j), a(k)));
            }
    return pts;
}

// Distance and colour together, because a combine couples them.
std::vector<float> sample(const scene::Tape& t, const std::vector<cfloat3>& pts) {
    std::vector<float> out;
    out.reserve(pts.size() * 4);
    for (cfloat3 p : pts) {
        const kernel::CTapeValue v = t.eval(p);
        out.push_back(v.d);
        out.push_back(v.color.x);
        out.push_back(v.color.y);
        out.push_back(v.color.z);
    }
    return out;
}

std::vector<float> sample(const scene::Document& doc, const std::vector<cfloat3>& pts) {
    return sample(scene::compile_document(doc), pts);
}

// A COUNT, not a vector comparison: doctest stringifies both operands of a
// failing CHECK and a lattice of thousands of points prints a megabyte.
int differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}

// -- documents ---------------------------------------------------------------

scene::Node sphere_at(float x, float r, cfloat3 colour) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    n.xform.position = cf3(x, 0.0f, 0.0f);
    n.color = colour;
    return n;
}

using scene::BlendProfile;

scene::LayerComposition composed(scene::Op op, BlendProfile profile = BlendProfile::Hard,
                                 float k = 0.0f, float rounding = 0.0f) {
    scene::LayerComposition c;
    c.op = op;
    c.blend.profile = profile;
    c.blend.k = k;
    c.rounding = rounding;
    return c;
}

// -- the C ABI refill harness, shared by the two invalidation gates ----------

constexpr int kDim = 8;
constexpr float kVox = 0.05f;

// Bricks along +x, from the origin outwards, so a brick index says how far from
// the centre it sits. Brick `i` covers x in [0.4i, 0.4(i+1)).
std::vector<clay_brick_request> axis_bricks(int count) {
    std::vector<clay_brick_request> reqs(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        clay_brick_request& r = reqs[static_cast<std::size_t>(i)];
        std::memset(&r, 0, sizeof r);
        r.key[0] = i;
        r.key[1] = -1;
        r.key[2] = -1;
        r.origin[0] = static_cast<float>(i) * kDim * kVox;
        r.origin[1] = -1.0f * kDim * kVox;
        r.origin[2] = -1.0f * kDim * kVox;
        r.spacing = kVox;
        r.dims[0] = kDim;
        r.dims[1] = kDim;
        r.dims[2] = kDim;
        r.band = 3.0f * kVox;
    }
    return reqs;
}

std::vector<float> refill(clay_document* d, const std::vector<clay_brick_request>& reqs) {
    const std::size_t per = kDim * kDim * kDim;
    std::vector<float> out(reqs.size() * per, 0.0f);
    REQUIRE(clay_brick_cache_eval_requests(d, nullptr, reqs.data(), reqs.size(), out.data(),
                                           out.size(), nullptr, 0) == CLAY_OK);
    bool near_surface = false;
    for (float v : out) near_surface = near_surface || std::fabs(v) < 0.5f;
    REQUIRE(near_surface);  // or a comparison is two readings of "far outside"
    return out;
}

void add_sphere_at(clay_document* d, clay_layer_id layer, float r, cfloat3 at) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {at.x, at.y, at.z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(d, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
}

void add_sphere(clay_document* d, clay_layer_id layer, float r, float x) {
    add_sphere_at(d, layer, r, cf3(x, 0.0f, 0.0f));
}

}  // namespace

// -- 6.1 hide and show a cutter ----------------------------------------------

TEST_CASE("gate: hiding a cutting layer restores exactly what it was cutting") {
    // The engine half. `test_layer_fold.cpp` holds the subtract case; this one
    // is the pair of operators together, and the point of it is that the
    // restored field is BIT-identical to a document that never carried the
    // cutter at all -- not merely close, and not merely "the cutter is gone".
    const std::vector<cfloat3> pts = lattice(16, 1.6f);

    auto build = [](scene::Op op, bool cutter_visible) {
        scene::Document doc;
        scene::Layer& base = doc.add_sdf_layer("base");
        base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.8f, 0.2f, 0.2f)));
        base.sdf->insert(sphere_at(0.7f, 0.5f, cf3(0.2f, 0.8f, 0.2f)));
        scene::Layer& cutter = doc.add_sdf_layer("cutter");
        cutter.sdf->insert(sphere_at(0.4f, 0.6f, cf3(0.2f, 0.2f, 0.8f)));
        cutter.composition = composed(op);
        cutter.visible = cutter_visible;
        return doc;
    };

    SUBCASE("a subtracting layer") {
        const std::vector<float> uncut = sample(build(scene::Op::Add, false), pts);
        const std::vector<float> cut = sample(build(scene::Op::Subtract, true), pts);
        const std::vector<float> restored = sample(build(scene::Op::Subtract, false), pts);
        CHECK(differing(restored, uncut) == 0);
        CHECK(differing(cut, uncut) > 0);  // it was really cutting
    }

    SUBCASE("an intersecting layer, which takes material away far from itself") {
        const std::vector<float> whole = sample(build(scene::Op::Add, false), pts);
        const std::vector<float> kept = sample(build(scene::Op::Intersect, true), pts);
        const std::vector<float> restored = sample(build(scene::Op::Intersect, false), pts);
        CHECK(differing(restored, whole) == 0);
        CHECK(differing(kept, whole) > 0);
    }
}

TEST_CASE("gate: a refill sees a cutter hidden, outside the cutter's own box") {
    // THE INVALIDATION HALF, and the reason it is a refill and not a compile.
    // The base sphere reaches x = 1.6; the intersecting layer reaches x = 0.5.
    // Bricks past x = 0.6 are therefore OUTSIDE the cutter's own extent, so a
    // dirty region taken from that extent alone leaves their seeds standing --
    // and `resume_bricks` copies a seed already at the current revision without
    // consulting a plan. With the layer visible those bricks read as empty
    // space; with it hidden the base's surface is in them. Nothing but the
    // width of the region decides which one the second call returns.
    //
    // THE THIRD LAYER IS LOAD-BEARING and is not decoration. A refill stores
    // no seed at all when the SEAM -- the last visible SDF layer -- is composed
    // (the split refusal, task 4.5), so a document whose top layer intersects
    // has nothing to keep stale and this gate would measure nothing. The cutter
    // therefore sits in the MIDDLE, under a plain unioning layer. That layer
    // also has to REACH these bricks: a seed whose brick reached no accumulator
    // is refused by the store, so a decorative layer parked off to the side
    // leaves `resumed_bricks` at zero and the gate measures nothing again
    // (measured: 0 resumed with it at y = 3, 3 resumed with it at the origin).
    const std::vector<clay_brick_request> reqs = axis_bricks(5);  // x in [0, 2)

    struct Doc {
        clay_document* d = nullptr;
        clay_layer_id base = 0, cutter = 0, top = 0;
        ~Doc() { clay_document_destroy(d); }
    };

    auto build = [&](Doc& doc, int32_t op, int32_t visible) {
        doc.d = clay_document_create();
        REQUIRE(doc.d != nullptr);
        REQUIRE(clay_add_sdf_layer(doc.d, "base", &doc.base) == CLAY_OK);
        add_sphere(doc.d, doc.base, 1.6f, 0.0f);
        REQUIRE(clay_add_sdf_layer(doc.d, "cutter", &doc.cutter) == CLAY_OK);
        add_sphere(doc.d, doc.cutter, 0.5f, 0.0f);
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, op, CLAY_BLEND_HARD, 0.0f,
                                                    0.0f) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(doc.d, "top", &doc.top) == CLAY_OK);
        add_sphere(doc.d, doc.top, 1.0f, 0.0f);
        if (!visible)
            REQUIRE(clay_document_set_layer_visible(doc.d, doc.cutter, 0) == CLAY_OK);
    };

    auto fresh = [&](int32_t op, int32_t visible) {
        Doc d;
        build(d, op, visible);
        return refill(d.d, reqs);
    };

    SUBCASE("an intersecting cutter, hidden after the bricks were filled") {
        Doc doc;
        build(doc, CLAY_OP_INTERSECT, 1);
        const std::vector<float> with_cutter = refill(doc.d, reqs);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.cutter, 0) == CLAY_OK);
        const std::vector<float> after_hiding = refill(doc.d, reqs);

        CHECK(differing(after_hiding, fresh(CLAY_OP_INTERSECT, 0)) == 0);
        CHECK(differing(after_hiding, with_cutter) > 0);  // hiding it did something

        SUBCASE("and shown again it is back, on the same seeds") {
            REQUIRE(clay_document_set_layer_visible(doc.d, doc.cutter, 1) == CLAY_OK);
            CHECK(differing(refill(doc.d, reqs), with_cutter) == 0);
        }
    }

    SUBCASE("a subtracting cutter, which the narrow region would have served") {
        // Here for contrast: a subtract cannot change anything outside its own
        // extent, so this arm passes whatever the region policy is. A gate that
        // tested only this would report the intersect case as covered.
        Doc doc;
        build(doc, CLAY_OP_SUBTRACT, 1);
        const std::vector<float> with_cutter = refill(doc.d, reqs);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.cutter, 0) == CLAY_OK);
        CHECK(differing(refill(doc.d, reqs), fresh(CLAY_OP_SUBTRACT, 0)) == 0);
        CHECK(differing(with_cutter, fresh(CLAY_OP_SUBTRACT, 0)) > 0);
    }

    SUBCASE("the bricks past the cutter really are outside its own box") {
        // The teeth. Without this, the case above could be passing because the
        // cutter happens to reach every brick it is compared over, which is the
        // arrangement in which the naive region is accidentally correct.
        Doc doc;
        build(doc, CLAY_OP_INTERSECT, 1);
        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        int32_t bounded = 0;
        REQUIRE(clay_layer_bounds(doc.d, doc.cutter, lo, hi, &bounded) == CLAY_OK);
        REQUIRE(bounded == 1);
        CHECK(hi[0] < 0.6f);
        const std::size_t per = kDim * kDim * kDim;
        const std::vector<float> visible = refill(doc.d, reqs);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.cutter, 0) == CLAY_OK);
        const std::vector<float> hidden = refill(doc.d, reqs);
        int outside_differs = 0;
        for (std::size_t b = 2; b < reqs.size(); ++b)  // bricks from x = 0.8 out
            for (std::size_t s = 0; s < per; ++s)
                if (visible[b * per + s] != hidden[b * per + s]) ++outside_differs;
        CHECK(outside_differs > 0);
    }
}

TEST_CASE("gate: a refill sees the BOTTOM layer hidden, outside that layer's box") {
    // THE OTHER HALF OF 6.1, and the one the gate above cannot see. There the
    // CUTTER is hidden; here the layer BENEATH it is -- which changes the field
    // by a different mechanism entirely.
    //
    // The first visible SDF layer initialises the accumulator and ITS OWN
    // OPERATOR IS NOT APPLIED, so hiding the bottom layer PROMOTES the cutter
    // to the initialiser: a cutter that was taking material away becomes the
    // shape itself. The base here is r 0.5 and the cutter r 1.6, so that
    // promotion changes the field out to x = 1.6 while the dirty region taken
    // from the base's own extent stops at 0.5. Bricks past it keep their seeds
    // AND have their revision advanced, so the next refill answers them from a
    // `below` half computed for a document that no longer exists. Measured with
    // the widening reverted: bricks 2, 3 and 4 come back resumed and unchanged,
    // where a fresh document moves all three by 0.1.
    //
    // THE TOP LAYER IS LOAD-BEARING TWICE OVER, and both halves were found by
    // measuring rather than by reasoning. It has to UNION, because a composed
    // seam stores no seed at all (task 4.5) and there would be nothing stale to
    // catch. And it has to be WIDE -- r 1.5, reaching every brick -- because
    // the promotion only ever turns empty space into material, and a brick that
    // held nothing before the edit is not answered from a lattice seed at all:
    // with a small top layer the outer bricks were empty, were refilled from
    // scratch, and the gate passed with the fix reverted.
    const std::vector<clay_brick_request> reqs = axis_bricks(5);  // x in [0, 2)

    struct Doc {
        clay_document* d = nullptr;
        clay_layer_id base = 0, cutter = 0, top = 0;
        ~Doc() { clay_document_destroy(d); }
    };

    auto build = [&](Doc& doc, int32_t op, int32_t base_visible) {
        doc.d = clay_document_create();
        REQUIRE(doc.d != nullptr);
        REQUIRE(clay_add_sdf_layer(doc.d, "base", &doc.base) == CLAY_OK);
        add_sphere(doc.d, doc.base, 0.5f, 0.0f);
        REQUIRE(clay_add_sdf_layer(doc.d, "cutter", &doc.cutter) == CLAY_OK);
        add_sphere(doc.d, doc.cutter, 1.6f, 0.0f);
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, op, CLAY_BLEND_HARD, 0.0f,
                                                    0.0f) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(doc.d, "top", &doc.top) == CLAY_OK);
        add_sphere(doc.d, doc.top, 1.5f, 0.0f);
        if (!base_visible)
            REQUIRE(clay_document_set_layer_visible(doc.d, doc.base, 0) == CLAY_OK);
    };

    for (int32_t op : {CLAY_OP_INTERSECT, CLAY_OP_SUBTRACT}) {
        CAPTURE(op);
        Doc doc;
        build(doc, op, 1);
        const std::vector<float> lit = refill(doc.d, reqs);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.base, 0) == CLAY_OK);
        const std::vector<float> after_hiding = refill(doc.d, reqs);

        Doc from_scratch;
        build(from_scratch, op, 0);
        CHECK(differing(after_hiding, refill(from_scratch.d, reqs)) == 0);
        CHECK(differing(after_hiding, lit) > 0);  // hiding it did something
    }

    SUBCASE("and a reorder that moves it off the bottom is the same flip") {
        // 6.2's invalidation half for the case its own gate cannot reach: a
        // reorder is a Remove+Add pair, each bounded by the MOVED layer's own
        // extent, so moving the BOTTOM layer up promotes the cutter exactly as
        // hiding it does -- and the moved layer's box is the same 0.5 that does
        // not reach the bricks the promotion changes.
        Doc doc;
        build(doc, CLAY_OP_SUBTRACT, 1);
        const std::vector<float> before = refill(doc.d, reqs);
        REQUIRE(clay_document_move_layer(doc.d, doc.base, 2) == CLAY_OK);  // to the top
        const std::vector<float> moved = refill(doc.d, reqs);

        // The same stack built in that order from scratch, which takes no
        // reorder and so shares none of the path under test.
        clay_document* fresh = clay_document_create();
        REQUIRE(fresh != nullptr);
        clay_layer_id cutter = 0, top = 0, base = 0;
        REQUIRE(clay_add_sdf_layer(fresh, "cutter", &cutter) == CLAY_OK);
        add_sphere(fresh, cutter, 1.6f, 0.0f);
        REQUIRE(clay_document_set_layer_composition(fresh, cutter, CLAY_OP_SUBTRACT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(fresh, "top", &top) == CLAY_OK);
        add_sphere(fresh, top, 1.5f, 0.0f);
        REQUIRE(clay_add_sdf_layer(fresh, "base", &base) == CLAY_OK);
        add_sphere(fresh, base, 0.5f, 0.0f);
        CHECK(differing(moved, refill(fresh, reqs)) == 0);
        CHECK(differing(moved, before) > 0);  // the move changed the field
        clay_document_destroy(fresh);
    }

    SUBCASE("and the change really is outside the base layer's own box") {
        // The teeth. Without this the case above could be passing on a fixture
        // where the base reaches every brick it is compared over, which is the
        // arrangement in which the un-widened region is accidentally right.
        Doc doc;
        build(doc, CLAY_OP_INTERSECT, 1);
        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        int32_t bounded = 0;
        REQUIRE(clay_layer_bounds(doc.d, doc.base, lo, hi, &bounded) == CLAY_OK);
        REQUIRE(bounded == 1);
        CHECK(hi[0] < 0.6f);
        const std::vector<float> lit = refill(doc.d, reqs);
        REQUIRE(clay_document_set_layer_visible(doc.d, doc.base, 0) == CLAY_OK);
        const std::vector<float> hidden = refill(doc.d, reqs);
        const std::size_t per = kDim * kDim * kDim;
        int outside_differs = 0;
        for (std::size_t b = 2; b < reqs.size(); ++b)  // bricks from x = 0.8 out
            for (std::size_t s = 0; s < per; ++s)
                if (lit[b * per + s] != hidden[b * per + s]) ++outside_differs;
        CHECK(outside_differs > 0);
    }
}

// -- 6.2 order matters, and survives a reload --------------------------------

namespace {

// A, then B and C in the order given. B subtracts and C unions, so
// A - B + C and A + C - B are the same three layers in two orders.
scene::Document ordered(bool cut_first) {
    scene::Document doc;
    scene::Layer& a = doc.add_sdf_layer("A");
    a.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.9f, 0.3f, 0.1f)));

    auto add_b = [&] {
        scene::Layer& b = doc.add_sdf_layer("B");
        b.sdf->insert(sphere_at(0.6f, 0.6f, cf3(0.1f, 0.1f, 0.1f)));
        b.composition = composed(scene::Op::Subtract);
    };
    auto add_c = [&] {
        scene::Layer& c = doc.add_sdf_layer("C");
        c.sdf->insert(sphere_at(0.9f, 0.5f, cf3(0.1f, 0.4f, 0.9f)));
        c.composition = composed(scene::Op::Add);
    };
    if (cut_first) {
        add_b();
        add_c();
    } else {
        add_c();
        add_b();
    }
    return doc;
}

}  // namespace

TEST_CASE("gate: A-B+C is not A+C-B, and each survives a save and a reload") {
    const std::vector<cfloat3> pts = lattice(16, 1.8f);
    const std::vector<float> cut_then_add = sample(ordered(true), pts);
    const std::vector<float> add_then_cut = sample(ordered(false), pts);

    // C sits where B cut, so folding C after B fills the notch back in and
    // folding it before B lets B cut both. Layer ORDER is geometry now.
    CHECK(differing(cut_then_add, add_then_cut) > 0);

    SUBCASE("and each order is bit-identical after a round trip") {
        for (bool cut_first : {true, false}) {
            CAPTURE(cut_first);
            const scene::Document doc = ordered(cut_first);
            const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
            const std::optional<scene::Document> back =
                scene::deserialize_document(bytes.data(), bytes.size());
            REQUIRE(back.has_value());
            CHECK(differing(sample(*back, pts),
                            cut_first ? cut_then_add : add_then_cut) == 0);
            // Order is what the reload has to keep, so say it directly too.
            REQUIRE(back->layers.size() == 3u);
            CHECK(back->layers[1].name == (cut_first ? "B" : "C"));
        }
    }

    SUBCASE("and reordering an existing document is the other order, exactly") {
        // Not a second document built differently: the same one, moved.
        scene::Document doc = ordered(true);
        const scene::LayerId b = doc.layers[1].id;
        scene::Layer moved = doc.layers[1];
        doc.layers.erase(doc.layers.begin() + 1);
        doc.layers.push_back(moved);
        CHECK(b == doc.layers[2].id);
        CHECK(differing(sample(doc, pts), add_then_cut) == 0);
    }
}

TEST_CASE("gate: a refill sees a reorder, outside the moved layer's own box") {
    // The invalidation half of 6.2, and the same argument as the hide gate: a
    // reorder is a Remove+Add pair, each bounded by the MOVED layer's extent,
    // and moving an intersecting layer changes the field everywhere the layers
    // it now folds onto have material.
    const std::vector<clay_brick_request> reqs = axis_bricks(5);

    struct Doc {
        clay_document* d = nullptr;
        clay_layer_id wide = 0, narrow = 0, cutter = 0;
        ~Doc() { clay_document_destroy(d); }
    };

    // wide (r 1.6) at the bottom, narrow (r 1.0) above it, and an intersecting
    // cutter (r 0.5) that is moved between them. Below `narrow` it intersects
    // `wide` alone; above it, both -- and the two differ out at x = 1.2, which
    // is a brick the cutter's own box never reaches.
    auto build = [&](Doc& doc, int32_t cutter_index) {
        doc.d = clay_document_create();
        REQUIRE(doc.d != nullptr);
        REQUIRE(clay_add_sdf_layer(doc.d, "wide", &doc.wide) == CLAY_OK);
        add_sphere(doc.d, doc.wide, 1.6f, 0.0f);
        REQUIRE(clay_add_sdf_layer(doc.d, "cutter", &doc.cutter) == CLAY_OK);
        add_sphere(doc.d, doc.cutter, 0.5f, 0.0f);
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.cutter, CLAY_OP_INTERSECT,
                                                    CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(doc.d, "narrow", &doc.narrow) == CLAY_OK);
        add_sphere(doc.d, doc.narrow, 1.0f, 0.0f);
        REQUIRE(clay_document_set_layer_composition(doc.d, doc.narrow, CLAY_OP_ADD, CLAY_BLEND_HARD,
                                                    0.0f, 0.0f) == CLAY_OK);
        if (cutter_index >= 0)
            REQUIRE(clay_document_move_layer(doc.d, doc.cutter, cutter_index) == CLAY_OK);
    };

    Doc built;
    build(built, -1);  // cutter in the middle
    const std::vector<float> middle = refill(built.d, reqs);
    REQUIRE(clay_document_move_layer(built.d, built.cutter, 2) == CLAY_OK);  // to the top
    const std::vector<float> moved = refill(built.d, reqs);

    Doc top;
    build(top, 2);
    CHECK(differing(moved, refill(top.d, reqs)) == 0);
    CHECK(differing(moved, middle) > 0);  // the move changed the field
}

// -- 6.3 an old document is unchanged ----------------------------------------

TEST_CASE("gate: a document written before compositions existed loads unioning") {
    // Minor 17 is the last layout with no composition block, so bytes written
    // at 17 are exactly what a pre-feature build wrote. The claim is not that
    // the value comes back as Add -- that is `test_layer_composition.cpp`'s --
    // but that the FIELD is what that document produced, bit for bit, which is
    // the fold reducing to the hard union it replaced.
    scene::Document doc;
    for (int i = 0; i < 3; ++i) {
        scene::Layer& l = doc.add_sdf_layer(i == 0 ? "a" : (i == 1 ? "b" : "c"));
        l.sdf->insert(sphere_at(0.6f * static_cast<float>(i) - 0.6f, 0.8f,
                                cf3(0.2f * static_cast<float>(i), 0.5f, 0.7f)));
        scene::Node blob = sphere_at(0.3f * static_cast<float>(i), 0.4f, cf3(0.9f, 0.9f, 0.1f));
        blob.op = scene::Op::Subtract;
        blob.blend.profile = scene::BlendProfile::Quadratic;
        blob.blend.k = 0.12f;
        l.sdf->insert(std::move(blob));
    }
    doc.layers[1].mirror_axes = 1;  // something for the layer record to carry

    const std::vector<std::uint8_t> old_bytes = scene::serialize_document(doc, 17);
    const std::optional<scene::Document> back =
        scene::deserialize_document(old_bytes.data(), old_bytes.size(), 17);
    REQUIRE(back.has_value());
    for (const scene::Layer& l : back->layers) {
        CAPTURE(l.name);
        CHECK(l.composition.op == scene::Op::Add);
        CHECK(l.composition.blend.profile == scene::BlendProfile::Hard);
        CHECK(l.composition.blend.k == 0.0f);
        CHECK(l.composition.rounding == 0.0f);
    }

    const scene::Tape now = scene::compile_document(doc);
    const scene::Tape reloaded = scene::compile_document(*back);
    const std::vector<cfloat3> pts = lattice(16, 1.6f);
    CHECK(differing(sample(reloaded, pts), sample(now, pts)) == 0);

    SUBCASE("and it is the same tape, not merely the same answers") {
        // The fold emits an instruction; a default composition has to emit the
        // instruction the hard union emitted, with the same parameters, or an
        // old document is a new tape that happens to agree at these points.
        REQUIRE(now.instrs.size() == reloaded.instrs.size());
        REQUIRE(now.params.size() == reloaded.params.size());
        CHECK(std::memcmp(now.instrs.data(), reloaded.instrs.data(),
                          now.instrs.size() * sizeof(now.instrs[0])) == 0);
        CHECK(std::memcmp(now.params.data(), reloaded.params.data(),
                          now.params.size() * sizeof(float)) == 0);
        CHECK(now.info.is_exact == reloaded.info.is_exact);
        CHECK(now.info.lipschitz == doctest::Approx(reloaded.info.lipschitz));
        CHECK(now.safe_step_scale() == doctest::Approx(reloaded.safe_step_scale()));
    }

    SUBCASE("and the bytes are what a build with no compositions would write") {
        // Both halves of the format gate in one line: a document where every
        // layer unions writes at 17 unchanged, so the reader above was reading
        // a genuinely old stream rather than a new one truncated.
        CHECK(old_bytes.size() < scene::serialize_document(doc).size());
    }
}

// -- 6.4 undo and redo -------------------------------------------------------

TEST_CASE("gate: undoing a composition restores the field, not only the value") {
    // `test_layer_composition.cpp` holds the value and the byte-level document
    // restore. What is left, now that the compiler reads the value, is that
    // the undone document COMPILES back to what it was.
    scene::Document doc;
    scene::Layer& base = doc.add_sdf_layer("base");
    base.sdf->insert(sphere_at(0.0f, 1.0f, cf3(0.7f, 0.3f, 0.3f)));
    scene::Layer& cutter = doc.add_sdf_layer("cutter");
    cutter.sdf->insert(sphere_at(0.5f, 0.6f, cf3(0.3f, 0.3f, 0.7f)));
    const scene::LayerId id = cutter.id;

    const std::vector<cfloat3> pts = lattice(16, 1.6f);
    const std::vector<float> unioned = sample(doc, pts);

    scene::UndoStack undo;
    REQUIRE(undo.perform(doc, scene::SetLayerCompositionCmd{
                                  id, composed(scene::Op::Subtract, scene::BlendProfile::Quadratic,
                                               0.15f, 0.03f)}));
    const std::vector<float> cut = sample(doc, pts);
    CHECK(differing(cut, unioned) > 0);

    REQUIRE(undo.undo(doc));
    CHECK(differing(sample(doc, pts), unioned) == 0);
    REQUIRE(undo.redo(doc));
    CHECK(differing(sample(doc, pts), cut) == 0);

    SUBCASE("and through the C ABI's own history, where a host meets it") {
        clay_document* d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_document_enable_undo(d) == CLAY_OK);
        clay_layer_id base_id = 0, cut_id = 0;
        REQUIRE(clay_add_sdf_layer(d, "base", &base_id) == CLAY_OK);
        add_sphere(d, base_id, 1.0f, 0.0f);
        REQUIRE(clay_add_sdf_layer(d, "cutter", &cut_id) == CLAY_OK);
        add_sphere(d, cut_id, 0.6f, 0.5f);

        const float probe[6] = {0.5f, 0.0f, 0.0f, 1.4f, 0.0f, 0.0f};
        float before[2] = {0.0f, 0.0f}, during[2] = {0.0f, 0.0f}, after[2] = {0.0f, 0.0f};
        REQUIRE(clay_eval_points(d, nullptr, probe, 2, before, nullptr) == CLAY_OK);
        REQUIRE(clay_document_set_layer_composition(d, cut_id, CLAY_OP_SUBTRACT, CLAY_BLEND_HARD,
                                                    0.0f, 0.0f) == CLAY_OK);
        REQUIRE(clay_eval_points(d, nullptr, probe, 2, during, nullptr) == CLAY_OK);
        CHECK(during[0] != before[0]);  // the cut is there

        int32_t stepped = 0;
        REQUIRE(clay_document_undo(d, &stepped) == CLAY_OK);
        CHECK(stepped == 1);
        REQUIRE(clay_eval_points(d, nullptr, probe, 2, after, nullptr) == CLAY_OK);
        CHECK(after[0] == before[0]);
        CHECK(after[1] == before[1]);
        clay_document_destroy(d);
    }
}

// -- 6.5 a converted mesh works as a cutter ----------------------------------

namespace {

mesh::Mesh sphere_mesh(float r, int rings) {
    mesh::Mesh m;
    const int segments = rings * 2;
    for (int i = 0; i <= rings; ++i) {
        const float phi = kPi * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j <= segments; ++j) {
            const float theta = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(segments);
            m.positions.push_back(cf3(r * std::sin(phi) * std::cos(theta), r * std::cos(phi),
                                      r * std::sin(phi) * std::sin(theta)));
        }
    }
    auto at = [&](int i, int j) { return static_cast<std::uint32_t>(i * (segments + 1) + j); };
    // Wound so the normals point outward: mesh::to_field takes its SIGN from the
    // winding number, so a reversed sphere is an inside-out field that still
    // compiles, still evaluates, and reads as a hollow world with a hole in it.
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            m.indices.insert(m.indices.end(), {at(i, j), at(i, j + 1), at(i + 1, j)});
            m.indices.insert(m.indices.end(), {at(i, j + 1), at(i + 1, j + 1), at(i + 1, j)});
        }
    return m;
}

}  // namespace

TEST_CASE("gate: an imported mesh, in its own layer, cuts the layers beneath it") {
    // A mesh reaches the tape as a sampled narrow-band volume, and until now
    // that volume had to live in the SAME layer as the thing it was cutting --
    // which is exactly the loss of organisation this change exists to end. The
    // claim is that a converted mesh is an ordinary layer: it composes, it
    // hides, and it agrees with the item form.
    const std::optional<field::FieldVolume> volume =
        mesh::to_field(sphere_mesh(0.55f, 24), {0.03f, 0.12f, 0.0f, 2.0f});
    REQUIRE(volume.has_value());
    const auto shared = std::make_shared<field::FieldVolume>(*volume);

    auto imported = [&](float x) {
        scene::Node n;
        n.prim = scene::Prim::volume();
        n.volume = shared;
        n.xform.position = cf3(x, 0.0f, 0.0f);
        n.color = cf3(0.1f, 0.7f, 0.4f);
        return n;
    };

    // Two layers: a base, and the imported mesh set to subtract.
    scene::Document layered;
    {
        scene::Layer& base = layered.add_sdf_layer("base");
        base.sdf->insert(sphere_at(0.0f, 0.9f, cf3(0.8f, 0.4f, 0.1f)));
        scene::Layer& cut = layered.add_sdf_layer("imported");
        cut.sdf->insert(imported(0.55f));
        cut.composition = composed(scene::Op::Subtract);
    }

    // The same shape the old way: both in one layer, the volume subtracting.
    scene::Document flat;
    {
        scene::Layer& l = flat.add_sdf_layer("l");
        l.sdf->insert(sphere_at(0.0f, 0.9f, cf3(0.8f, 0.4f, 0.1f)));
        scene::Node cut = imported(0.55f);
        cut.op = scene::Op::Subtract;
        l.sdf->insert(std::move(cut));
    }

    const std::vector<cfloat3> pts = lattice(16, 1.4f);
    CHECK(differing(sample(layered, pts), sample(flat, pts)) == 0);

    SUBCASE("and it is really carving, not agreeing about empty space") {
        scene::Document uncut = layered;
        uncut.layers[1].visible = false;
        const std::vector<float> open = sample(uncut, pts);
        CHECK(differing(sample(layered, pts), open) > 0);

        // A point inside the imported shell and inside the base: solid before
        // the layer folds, open after it. Taken inside the volume's BAND -- a
        // narrow-band volume clamps to its band away from the surface, so the
        // deep interior of an imported mesh reads as band-deep rather than as
        // the true distance, and a probe there measures the import's storage
        // rather than the fold.
        const scene::Tape carved = scene::compile_document(layered);
        const scene::Tape whole = scene::compile_document(uncut);
        const cfloat3 inside_both = cf3(0.1f, 0.0f, 0.0f);
        CHECK(whole.eval(inside_both).d < 0.0f);   // solid before
        CHECK(carved.eval(inside_both).d > 0.0f);  // open after
    }

    SUBCASE("and hiding the imported layer gives the uncarved form back exactly") {
        scene::Document without = layered;
        without.layers.pop_back();
        scene::Document hidden = layered;
        hidden.layers[1].visible = false;
        CHECK(differing(sample(hidden, pts), sample(without, pts)) == 0);
    }
}

// -- 6.6 the count half: one combine per fold, and the same one --------------

TEST_CASE("gate: a layer fold emits the combine a group fold emits, and no more") {
    // The clock half of 6.6 is `BM_LayerFoldStack*` against `BM_ItemFoldStack*`
    // in benchmarks/bench_main.cpp. "There is no second evaluator" is a COUNT,
    // so it is asserted as one, here: the same items folded in N chunks as
    // separate LAYERS and as GROUPS inside one layer compile to the same number
    // of instructions, because a layer's fold IS the combine a group's tail
    // emits. A change that added a layer-level emitter beside the item one
    // would read one extra instruction per fold, which no wall clock can see
    // among thousands of items.
    constexpr int kItems = 240;

    auto build = [](int chunks, bool as_layers) {
        scene::Document doc;
        const int per = kItems / chunks;
        scene::Layer* one = as_layers ? nullptr : &doc.add_sdf_layer("all");
        for (int c = 0; c < chunks; ++c) {
            scene::Layer* target = one;
            scene::NodeId parent = scene::kNoNode;
            if (as_layers) {
                target = &doc.add_sdf_layer("chunk");
                if (c > 0) target->composition = composed(scene::Op::Subtract);
            } else if (c > 0) {
                scene::Node group;
                group.is_group = true;
                group.op = scene::Op::Subtract;
                parent = one->sdf->insert(group);
            }
            for (int i = 0; i < per; ++i) {
                const int n = c * per + i;
                const float t = static_cast<float>(n) / kItems;
                scene::Node dab = sphere_at(1.4f * t - 0.7f, 0.2f, cf3(t, 1.0f - t, 0.5f));
                dab.xform.position.y = 0.3f * std::sin(11.0f * t);
                if (parent == scene::kNoNode)
                    target->sdf->insert(dab);
                else
                    target->sdf->insert(dab, parent);
            }
        }
        return doc;
    };

    for (int chunks : {4, 16, 60}) {
        CAPTURE(chunks);
        const scene::Tape layers = scene::compile_document(build(chunks, true));
        const scene::Tape groups = scene::compile_document(build(chunks, false));
        CHECK(layers.instrs.size() == groups.instrs.size());
        CHECK(layers.params.size() == groups.params.size());
    }

    SUBCASE("and a fold is ONE instruction, wherever the chunk boundary falls") {
        // The sharper form of the same claim, and the teeth under it: the total
        // is 2N-1 for N items at EVERY chunking -- one instruction per item,
        // plus one combine per item after the first, whether that combine sits
        // inside a chain, at a group's tail or at a layer fold. A layer fold
        // does not ADD an instruction; it is the one the chain would have
        // emitted anyway, with a different operator. A second emitter would
        // show up as `chunks - 1` extra.
        for (int chunks : {4, 16, 60}) {
            CAPTURE(chunks);
            CHECK(scene::compile_document(build(chunks, true)).instrs.size() ==
                  2u * kItems - 1u);
        }
        // 2N-1 is PREDICTED rather than measured-and-copied, which is what
        // makes this the teeth: a form that emitted a second combine per fold
        // could not land on it at three different chunkings.
    }
}

