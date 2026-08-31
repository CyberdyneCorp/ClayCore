#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "clay.h"

// The Move brush across the C ABI (c-abi spec, add-move-brush).

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

clay_move_params move_params(float radius) {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.radius = radius;
    return p;
}

// Two balls smooth-unioned: the case a per-item grab gets wrong.
clay_layer_id blended_form(clay_document* doc) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "form", &layer) == CLAY_OK);
    for (float x : {-0.45f, 0.45f}) {
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_SPHERE;
        d.params[0] = 0.5f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.25f;
        d.position[0] = x;
        clay_node_id node = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &node) == CLAY_OK);
    }
    return layer;
}

float top_at(clay_document* doc, float x) {
    float last = 1.0f;
    for (float y = 1.6f; y > -1.6f; y -= 0.002f) {
        const float point[3] = {x, y, 0.0f};
        float d = 0.0f;
        REQUIRE(clay_eval_points(doc, nullptr, point, 1, &d, nullptr) == CLAY_OK);
        if (d <= 0.0f && last > 0.0f) return y;
        last = d;
    }
    return 0.0f;
}

}  // namespace

TEST_CASE("c move: a drag moves the assembled surface, symmetrically") {
    CDoc base;
    const clay_layer_id base_layer = blended_form(base.doc);
    (void)base_layer;
    const float before_left = top_at(base.doc, -0.45f);
    const float before_centre = top_at(base.doc, 0.0f);

    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};
    const clay_move_params p = move_params(0.8f);
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(c.doc, layer, centre, displacement, &p, &applied) ==
            CLAY_OK);
    CHECK(applied == 2);  // both items took a share

    const float left = top_at(c.doc, -0.45f) - before_left;
    const float right = top_at(c.doc, 0.45f) - before_left;  // symmetric form
    const float middle = top_at(c.doc, 0.0f) - before_centre;
    CHECK(left > 0.0f);
    CHECK(left == doctest::Approx(right).epsilon(0.1));
    CHECK(middle >= left);
    CHECK(middle < 0.4f);  // grab pulls short of the displacement, by design
}

TEST_CASE("c move: the whole drag is one undo step") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    REQUIRE(clay_document_enable_undo(c.doc) == CLAY_OK);
    const float before = top_at(c.doc, 0.0f);

    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};
    const clay_move_params p = move_params(0.8f);
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(c.doc, layer, centre, displacement, &p, &applied) ==
            CLAY_OK);
    REQUIRE(applied == 2);
    CHECK(top_at(c.doc, 0.0f) > before);

    int32_t enabled = 0;
    size_t depth = 0, redo = 0;
    REQUIRE(clay_document_undo_state(c.doc, &enabled, &depth, &redo) == CLAY_OK);
    CHECK(depth == 1);  // two items, one gesture, one step

    int32_t undone = 0;
    REQUIRE(clay_document_undo(c.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));
}

TEST_CASE("c move: a drag that reaches nothing succeeds and changes nothing") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float before = top_at(c.doc, 0.0f);
    const float far[3] = {40.0f, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};
    const clay_move_params p = move_params(0.8f);
    size_t applied = 99;
    CHECK(clay_layer_move_surface(c.doc, layer, far, displacement, &p, &applied) == CLAY_OK);
    CHECK(applied == 0);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));
}

TEST_CASE("c move: bad arguments are refused") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.4f, 0};

    clay_move_params bad = move_params(0.0f);
    CHECK(clay_layer_move_surface(c.doc, layer, centre, displacement, &bad, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    bad = move_params(0.8f);
    bad.struct_size = 4;  // below the original layout
    CHECK(clay_layer_move_surface(c.doc, layer, centre, displacement, &bad, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    const clay_move_params good = move_params(0.8f);
    CHECK(clay_layer_move_surface(c.doc, 999, centre, displacement, &good, nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_move_surface(c.doc, layer, nullptr, displacement, &good, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_move_surface(c.doc, layer, centre, displacement, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c move: a deformer can be added to a node already in a document") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float before = top_at(c.doc, -0.45f);

    // The mutation nothing could do before: clay_item_add_deformer builds an
    // item, this edits a placed one.
    // centre(3), radius, displacement(3), front_only — eight, per kDeformParams.
    const float params[8] = {0.0f, 0.0f, 0.0f, 0.8f, 0.0f, 0.4f, 0.0f, 0.0f};
    REQUIRE(clay_layer_add_deformer(c.doc, layer, 1, CLAY_DEFORM_GRAB, params, 8, 0, 1) ==
            CLAY_OK);
    CHECK(top_at(c.doc, -0.45f) > before);

    CHECK(clay_layer_add_deformer(c.doc, layer, 9999, CLAY_DEFORM_GRAB, params, 8, 0, 1) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_add_deformer(c.doc, layer, 1, 999, params, 8, 0, 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c move: magnify and noise are reachable from C at all") {
    // Regression. The deformer bound check stopped at POSE_LINE, so the two
    // kinds added after it were declared, documented, given parameter counts
    // and handled by the decoder — and refused at the door. Python could reach
    // them; C could not, and the parity gate checks enumerators rather than
    // calls, so nothing noticed.
    const float radius[1] = {0.5f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(item != nullptr);
    const float magnify[5] = {0.0f, 0.0f, 0.0f, 0.4f, 0.5f};
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_MAGNIFY, magnify, 5, 0) == CLAY_OK);
    const float noise[5] = {0.15f, 2.0f, 3.0f, 0.5f, 7.0f};
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_NOISE, noise, 5, 0) == CLAY_OK);
    // ...and one past the end is still refused.
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_NOISE + 1, noise, 5, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(item);
}

TEST_CASE("c abi: every declared deformer kind is actually reachable") {
    // The general form of the bug above, which is what stops the NEXT deformer
    // being declared, documented and refused at the door. Naming the two that
    // were broken would not have caught them before they were broken; walking
    // the whole enumeration does.
    //
    // Parameter counts are the ones clay.h documents for each kind. If a new
    // deformer lands without extending the bound, or with a count the boundary
    // disagrees with, this fails and says which.
    // Each kind gets parameters that are VALID for it, not one generic array:
    // several validate their contents as well as their count — noise refuses
    // fewer than one octave, magnify refuses a radius that is not > 0 — so a
    // shared array would report "unreachable" for what is really a bad value.
    struct Kind {
        std::int32_t deform;
        const char* name;
        int count;
        float params[10];
    };
    const Kind kinds[] = {
        {CLAY_DEFORM_TWIST, "twist", 1, {1.2f}},
        {CLAY_DEFORM_BEND, "bend", 1, {0.8f}},
        {CLAY_DEFORM_TAPER, "taper", 4, {-0.5f, 0.5f, 1.0f, 0.4f}},
        {CLAY_DEFORM_DISPLACE, "displace", 2, {0.1f, 4.0f}},
        {CLAY_DEFORM_WRAP_AROUND, "wrap_around", 2, {-0.5f, 0.5f}},
        {CLAY_DEFORM_ELONGATE, "elongate", 3, {0.2f, 0.1f, 0.0f}},
        {CLAY_DEFORM_BEND_LINEAR, "bend_linear", 9,
         {0.0f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f}},
        {CLAY_DEFORM_BEND_RADIAL, "bend_radial", 3, {0.2f, 0.6f, 0.3f}},
        {CLAY_DEFORM_ELONGATE_AXIS, "elongate_axis", 3, {0.2f, 0.1f, 0.0f}},
        {CLAY_DEFORM_GRAB, "grab", 8, {0.0f, 0.3f, 0.0f, 0.5f, 0.1f, 0.2f, 0.0f, 0.0f}},
        {CLAY_DEFORM_POSE, "pose", 8, {0.0f, 0.3f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.6f}},
        {CLAY_DEFORM_POSE_LINE, "pose_line", 10,
         {0.0f, -0.4f, 0.0f, 0.0f, 0.4f, 0.0f, 0.0f, 1.0f, 0.0f, 0.7f}},
        {CLAY_DEFORM_MAGNIFY, "magnify", 5, {0.0f, 0.0f, 0.0f, 0.4f, 0.5f}},
        {CLAY_DEFORM_NOISE, "noise", 5, {0.15f, 2.0f, 4.0f, 0.5f, 7.0f}},
    };
    // Every enumerator from 0 to the last is covered, so a new kind added
    // without a row here fails this rather than slipping through untested.
    REQUIRE(static_cast<int>(sizeof kinds / sizeof kinds[0]) == CLAY_DEFORM_NOISE + 1);

    const float radius[1] = {0.5f};
    for (const Kind& k : kinds) {
        CAPTURE(k.deform);
        INFO("kind: " << k.name);
        clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
        REQUIRE(item != nullptr);
        CHECK(clay_item_add_deformer(item, k.deform, k.params, k.count, 0) == CLAY_OK);
        // ...and the documented count is the one the boundary enforces.
        CHECK(clay_item_add_deformer(item, k.deform, k.params, k.count + 1, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_item_destroy(item);
    }
}

TEST_CASE("c move: previewing a drag names the nodes and touches nothing") {
    CDoc doc;
    const clay_layer_id layer = blended_form(doc.doc);
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    const float displacement[3] = {0.0f, 0.35f, 0.0f};
    clay_move_params p = move_params(1.2f);

    std::size_t count = 0;
    REQUIRE(clay_layer_move_surface_preview(doc.doc, layer, centre, displacement, &p, nullptr,
                                            0, &count) == CLAY_OK);
    CHECK(count == 2);

    clay_node_id nodes[2] = {0, 0};
    REQUIRE(clay_layer_move_surface_preview(doc.doc, layer, centre, displacement, &p, nodes,
                                            2, &count) == CLAY_OK);
    CHECK(nodes[0] != nodes[1]);

    // The document is untouched: the same probe reads the same before and after.
    const float probe[3] = {0.0f, 0.5f, 0.0f};
    float before = 0.0f, after = 0.0f;
    REQUIRE(clay_eval_points(doc.doc, nullptr, probe, 1, &before, nullptr) == CLAY_OK);
    REQUIRE(clay_layer_move_surface_preview(doc.doc, layer, centre, displacement, &p, nodes, 2,
                                            &count) == CLAY_OK);
    REQUIRE(clay_eval_points(doc.doc, nullptr, probe, 1, &after, nullptr) == CLAY_OK);
    CHECK(before == doctest::Approx(after));

    // ...and the preview agrees with what the move then does.
    std::size_t applied = 0;
    REQUIRE(clay_layer_move_surface(doc.doc, layer, centre, displacement, &p, &applied) ==
            CLAY_OK);
    CHECK(applied == count);
}

// -- a drag under symmetry (#363) ----------------------------------------------
// Under a layer mirror the move used to select on the item's MIRROR-EXPANDED
// bound: every participating item's bound spanned the plane, so a grab on a
// ridge at x 1.45 also took the base ball and dabs on both sides -- 46 items
// where the unmirrored drag took 22, and the base (ordinal 0) in every drag,
// which is what kept the dirty-prefix path (#360) from ever engaging under a
// mirror. The brush is now reflected instead of the bound, so a mirrored drag
// selects exactly what the ball or its reflection touches.

namespace {

// The fixture of #363: a unit base ball, 300 dabs over the +x hemisphere
// (abi_sculpt's placement, bench_main.cpp), and a 24-ball ridge sticking out
// at x 1.45 appended LAST -- the items a grab on its crest should take, and
// the only ones. Everything sits at x >= 0, so the reflected ball at x -1.53
// reaches nothing and the mirrored selection has to equal the unmirrored one.
struct Ridge {
    clay_layer_id layer = 0;
    std::vector<clay_node_id> ridge;
};

clay_node_id ridge_ball(clay_document* doc, clay_layer_id layer, float r, float x, float y, float z,
                        float k) {
    clay_item_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<uint32_t>(sizeof d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = r;
    d.op = CLAY_OP_ADD;
    d.blend = k > 0.0f ? CLAY_BLEND_QUADRATIC : CLAY_BLEND_HARD;
    d.blend_k = k;
    d.position[0] = x;
    d.position[1] = y;
    d.position[2] = z;
    d.rotation[3] = 1.0f;
    d.scale = 1.0f;
    clay_node_id id = 0;
    REQUIRE(clay_add_item(doc, layer, &d, &id) == CLAY_OK);
    return id;
}

Ridge ridge_sculpt(clay_document* doc, bool mirrored) {
    Ridge r;
    REQUIRE(clay_add_sdf_layer(doc, "ridge", &r.layer) == CLAY_OK);
    ridge_ball(doc, r.layer, 1.0f, 0, 0, 0, 0.0f);
    const int nodes = 301;
    for (int i = 1; i < nodes; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / nodes;
        const double rr = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double th = 2.399963 * i;
        const double a = rr * std::cos(th), b = rr * std::sin(th);
        ridge_ball(doc, r.layer, 0.05f,
                   static_cast<float>(std::sqrt(std::max(0.0, 1.0 - a * a - b * b))),
                   static_cast<float>(a), static_cast<float>(b), 0.0f);
    }
    for (int j = 0; j < 24; ++j)
        r.ridge.push_back(ridge_ball(doc, r.layer, 0.08f, 1.45f, 0,
                                     (static_cast<float>(j) - 11.5f) * 0.055f, 0.05f));
    if (mirrored) REQUIRE(clay_set_layer_mirror(doc, r.layer, 1, 0, 0, 0.05f) == CLAY_OK);
    return r;
}

std::vector<clay_node_id> preview_nodes(clay_document* doc, clay_layer_id layer,
                                        const float centre[3], const float disp[3],
                                        const clay_move_params& p) {
    std::size_t count = 0;
    REQUIRE(clay_layer_move_surface_preview(doc, layer, centre, disp, &p, nullptr, 0, &count) ==
            CLAY_OK);
    std::vector<clay_node_id> nodes(count);
    REQUIRE(clay_layer_move_surface_preview(doc, layer, centre, disp, &p, nodes.data(), count,
                                            &count) == CLAY_OK);
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

constexpr float kCrest[3] = {1.53f, 0.0f, 0.0f};
constexpr float kCrestRadius = 0.35f;

}  // namespace

TEST_CASE("c move: under a mirror the drag selects what the ball touches, not the plane") {
    // Acceptance (1) and (5) of #363. Compared as SETS, because the count is
    // a property of the ridge spacing: the point is that the mirror adds
    // nothing here and the base is not among the selected. Reverting the
    // selection to the mirror-expanded bound re-selects node 1 (the base) and
    // 24 more items off the ridge.
    const float disp[3] = {0.03f, 0.0f, 0.0f};
    const clay_move_params p = move_params(kCrestRadius);

    CDoc plain;
    const Ridge a = ridge_sculpt(plain.doc, false);
    const std::vector<clay_node_id> unmirrored = preview_nodes(plain.doc, a.layer, kCrest, disp, p);

    CDoc mirrored;
    const Ridge b = ridge_sculpt(mirrored.doc, true);
    const std::vector<clay_node_id> under_mirror =
        preview_nodes(mirrored.doc, b.layer, kCrest, disp, p);

    REQUIRE_FALSE(unmirrored.empty());
    CHECK(under_mirror == unmirrored);
    // Every selected node is on the ridge, and the base is not among them.
    for (clay_node_id id : under_mirror) {
        CAPTURE(id);
        CHECK(std::find(b.ridge.begin(), b.ridge.end(), id) != b.ridge.end());
    }
    CHECK(std::find(under_mirror.begin(), under_mirror.end(), 1u) == under_mirror.end());
}

TEST_CASE("c move: a mirrored gesture accumulates the warps of an unmirrored one") {
    // Acceptance (2). Twelve segments along the ridge, each a fresh centre so
    // nothing coalesces: the mirrored gesture used to leave 532 warps where
    // the unmirrored one left 244 (2.18x), each a grab evaluated per sample on
    // every later refill. On this fixture the reflected ball reaches nothing,
    // so the two totals are now EQUAL; the 2x bound is the acceptance line.
    const clay_move_params p = move_params(kCrestRadius);
    const float disp[3] = {0.05f, 0.0f, 0.0f};
    std::size_t totals[2] = {0, 0};
    for (int m = 0; m < 2; ++m) {
        CDoc doc;
        const Ridge r = ridge_sculpt(doc.doc, m == 1);
        for (int s = 0; s < 12; ++s) {
            const float centre[3] = {1.53f, 0.0f, -0.33f + 0.06f * static_cast<float>(s)};
            std::size_t applied = 0;
            REQUIRE(clay_layer_move_surface(doc.doc, r.layer, centre, disp, &p, &applied) ==
                    CLAY_OK);
            totals[m] += applied;
        }
    }
    REQUIRE(totals[0] > 0);
    CHECK(totals[1] == totals[0]);
    CHECK(totals[1] <= 2 * totals[0]);
}

TEST_CASE("c move: a mirrored preview names what the move applies, and a straddler once") {
    // *out_applied and the preview count ITEMS. An item both the ball and its
    // reflection reach -- one sitting on the plane -- takes one grab per
    // image inside a single SetDeformersCmd, so it is reported once by both.
    CDoc doc;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc.doc, "s", &layer) == CLAY_OK);
    ridge_ball(doc.doc, layer, 0.4f, 0, 0, 0, 0.05f);          // base, out of this reach
    ridge_ball(doc.doc, layer, 0.2f, 0, 1.5f, 0, 0.05f);       // straddler on the plane
    ridge_ball(doc.doc, layer, 0.2f, 1.0f, 0.3f, 0, 0.05f);    // far from this drag
    REQUIRE(clay_set_layer_mirror(doc.doc, layer, 1, 0, 0, 0.05f) == CLAY_OK);

    const float centre[3] = {0.25f, 1.5f, 0.0f};  // both images reach the straddler
    const float disp[3] = {0.15f, 0.0f, 0.0f};
    const clay_move_params p = move_params(0.35f);
    const std::vector<clay_node_id> previewed = preview_nodes(doc.doc, layer, centre, disp, p);
    CHECK(previewed == std::vector<clay_node_id>{2});  // the straddler, once

    std::size_t applied = 0;
    REQUIRE(clay_layer_move_surface(doc.doc, layer, centre, disp, &p, &applied) == CLAY_OK);
    CHECK(applied == previewed.size());
}

TEST_CASE("c move: a preview refuses what the move refuses") {
    CDoc doc;
    const clay_layer_id layer = blended_form(doc.doc);
    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.3f, 0};
    std::size_t count = 0;

    clay_move_params bad = move_params(0.0f);  // radius must be > 0
    CHECK(clay_layer_move_surface_preview(doc.doc, layer, centre, displacement, &bad, nullptr,
                                          0, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_move_params p = move_params(1.0f);
    CHECK(clay_layer_move_surface_preview(doc.doc, 4242, centre, displacement, &p, nullptr, 0,
                                          &count) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_move_surface_preview(doc.doc, layer, centre, displacement, &p, nullptr, 0,
                                          nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    p.struct_size = 0;
    CHECK(clay_layer_move_surface_preview(doc.doc, layer, centre, displacement, &p, nullptr, 0,
                                          &count) == CLAY_ERROR_INVALID_ARGUMENT);
}

// -- magnify / pinch on the assembled surface (issue #391) --------------------
//
// CLAY_DEFORM_MAGNIFY is per item and local exactly as grab is, and until
// clay_layer_magnify_surface there was no counterpart to clay_layer_move_surface
// for it — so Pinch could not be a surface brush on a field from C at all. The
// ABI note above ("magnify and noise are reachable from C at all") covered the
// per-item deformer, which is the thing that gets the blend wrong.

namespace {

clay_magnify_params magnify_params(float radius) {
    clay_magnify_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.radius = radius;
    return p;
}

}  // namespace

TEST_CASE("c magnify: a gesture swells the assembled surface, symmetrically") {
    CDoc base;
    (void)blended_form(base.doc);
    const float before_left = top_at(base.doc, -0.45f);

    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float centre[3] = {0, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);
    size_t applied = 0;
    REQUIRE(clay_layer_magnify_surface(c.doc, layer, centre, 0.4f, &p, &applied) == CLAY_OK);
    CHECK(applied == 2);  // both items took a share, which is the whole point

    const float left = top_at(c.doc, -0.45f) - before_left;
    const float right = top_at(c.doc, 0.45f) - before_left;  // symmetric form
    CHECK(left > 0.0f);
    CHECK(left == doctest::Approx(right).epsilon(0.1));
}

TEST_CASE("c magnify: the sign is the difference between Magnify and Pinch") {
    CDoc base;
    (void)blended_form(base.doc);
    const float before = top_at(base.doc, 0.0f);
    const float centre[3] = {0, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);

    CDoc swelled;
    const clay_layer_id a = blended_form(swelled.doc);
    REQUIRE(clay_layer_magnify_surface(swelled.doc, a, centre, 0.4f, &p, nullptr) == CLAY_OK);
    CHECK(top_at(swelled.doc, 0.0f) > before);

    CDoc gathered;
    const clay_layer_id b = blended_form(gathered.doc);
    REQUIRE(clay_layer_magnify_surface(gathered.doc, b, centre, -0.4f, &p, nullptr) == CLAY_OK);
    CHECK(top_at(gathered.doc, 0.0f) < before);
}

TEST_CASE("c magnify: the whole gesture is one undo step") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    REQUIRE(clay_document_enable_undo(c.doc) == CLAY_OK);
    const float before = top_at(c.doc, 0.0f);

    const float centre[3] = {0, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);
    size_t applied = 0;
    REQUIRE(clay_layer_magnify_surface(c.doc, layer, centre, 0.4f, &p, &applied) == CLAY_OK);
    REQUIRE(applied == 2);
    CHECK(top_at(c.doc, 0.0f) > before);

    int32_t enabled = 0;
    size_t depth = 0, redo = 0;
    REQUIRE(clay_document_undo_state(c.doc, &enabled, &depth, &redo) == CLAY_OK);
    CHECK(depth == 1);  // two items, one gesture, one step

    int32_t undone = 0;
    REQUIRE(clay_document_undo(c.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));
}

TEST_CASE("c magnify: a live gesture replaces its own last frame") {
    // A host calls this every frame with a growing strength. Stacking one
    // deformer per frame would grow the chain without bound and compound the
    // declared Lipschitz with it.
    CDoc live;
    const clay_layer_id layer = blended_form(live.doc);
    const float centre[3] = {0, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);
    for (float strength : {0.1f, 0.2f, 0.3f, 0.4f})
        REQUIRE(clay_layer_magnify_surface(live.doc, layer, centre, strength, &p, nullptr) ==
                CLAY_OK);

    CDoc once;
    const clay_layer_id other = blended_form(once.doc);
    REQUIRE(clay_layer_magnify_surface(once.doc, other, centre, 0.4f, &p, nullptr) == CLAY_OK);

    // The same document either way, sampled across the gesture.
    for (float x = -1.2f; x <= 1.2f; x += 0.1f)
        CHECK(top_at(live.doc, x) == doctest::Approx(top_at(once.doc, x)));
}

TEST_CASE("c magnify: a gesture that reaches nothing succeeds and changes nothing") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float before = top_at(c.doc, 0.0f);
    const float far[3] = {40.0f, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);
    size_t applied = 99;
    CHECK(clay_layer_magnify_surface(c.doc, layer, far, 0.4f, &p, &applied) == CLAY_OK);
    CHECK(applied == 0);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));
}

TEST_CASE("c magnify: bad arguments are refused") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float centre[3] = {0, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);

    CHECK(clay_layer_magnify_surface(nullptr, layer, centre, 0.4f, &p, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_magnify_surface(c.doc, layer, nullptr, 0.4f, &p, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_magnify_surface(c.doc, layer, centre, 0.4f, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_magnify_surface(c.doc, 999, centre, 0.4f, &p, nullptr) ==
          CLAY_ERROR_NOT_FOUND);

    // A strength of zero scales by one: not a gesture, refused rather than
    // accepted as a silent no-op.
    CHECK(clay_layer_magnify_surface(c.doc, layer, centre, 0.0f, &p, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_magnify_params bad = magnify_params(0.0f);
    CHECK(clay_layer_magnify_surface(c.doc, layer, centre, 0.4f, &bad, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_magnify_params stale = magnify_params(0.8f);
    stale.struct_size = 0;
    CHECK(clay_layer_magnify_surface(c.doc, layer, centre, 0.4f, &stale, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c magnify: previewing names the nodes and touches nothing") {
    CDoc c;
    const clay_layer_id layer = blended_form(c.doc);
    const float before = top_at(c.doc, 0.0f);
    const float centre[3] = {0, 0, 0};
    const clay_magnify_params p = magnify_params(0.8f);

    size_t count = 0;
    REQUIRE(clay_layer_magnify_surface_preview(c.doc, layer, centre, 0.4f, &p, nullptr, 0,
                                               &count) == CLAY_OK);
    CHECK(count == 2);

    std::vector<clay_node_id> nodes(count, 0);
    REQUIRE(clay_layer_magnify_surface_preview(c.doc, layer, centre, 0.4f, &p, nodes.data(),
                                               nodes.size(), &count) == CLAY_OK);
    CHECK(count == 2);
    CHECK(nodes[0] != nodes[1]);
    CHECK(top_at(c.doc, 0.0f) == doctest::Approx(before));  // pure

    // ...and it refuses what the apply refuses, so a host does not discover a
    // bad gesture only on commit.
    CHECK(clay_layer_magnify_surface_preview(c.doc, layer, centre, 0.0f, &p, nullptr, 0,
                                             &count) == CLAY_ERROR_INVALID_ARGUMENT);

    // Then the apply names the same nodes.
    size_t applied = 0;
    REQUIRE(clay_layer_magnify_surface(c.doc, layer, centre, 0.4f, &p, &applied) == CLAY_OK);
    CHECK(applied == count);
}

// THE WARP BUFFER IS REUSED ACROSS ITEMS, and this pins that it is reset.
//
// `apply_surface_gesture` resolves one warp at a time into a single MoveWarp
// rather than materialising a vector of them, which is what took the drag's
// per-item allocations from 6.16 to 5.10 (#375). The buffer is only safe
// because both resolvers write `node` and clear `deformers` and `gesture`
// before filling them — so an item reached by ONE image cannot inherit a
// second grab from the previous item, and no item can be applied under a
// stale node id.
//
// A drag over many items at different distances is what makes that visible:
// the items nearest the centre take the strongest pull and the ones at the rim
// the weakest, so a leaked grab or a stale node shows up as an item moving by
// the wrong neighbour's share. Asserted against the SAME drag applied to a
// document holding each item alone, which cannot reuse anything.
TEST_CASE("c move: a many-item drag resolves each item from its own share") {
    // A row of separate balls, far enough apart not to blend, spanning the
    // drag's radius so their shares of the pull genuinely differ.
    const float xs[] = {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f};
    auto build = [](clay_document* doc, std::initializer_list<float> at) {
        clay_layer_id layer = 0;
        REQUIRE(clay_add_sdf_layer(doc, "row", &layer) == CLAY_OK);
        for (float x : at) {
            clay_item_desc d;
            std::memset(&d, 0, sizeof d);
            d.struct_size = static_cast<uint32_t>(sizeof d);
            d.prim = CLAY_PRIM_SPHERE;
            d.params[0] = 0.12f;
            d.op = CLAY_OP_ADD;
            d.position[0] = x;
            clay_node_id node = 0;
            REQUIRE(clay_add_item(doc, layer, &d, &node) == CLAY_OK);
        }
        return layer;
    };

    const float centre[3] = {0, 0, 0};
    const float displacement[3] = {0, 0.25f, 0};
    const clay_move_params p = move_params(0.9f);

    // All five in one document: five items resolved through one reused buffer.
    CDoc together;
    const clay_layer_id all = build(together.doc, {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f});
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(together.doc, all, centre, displacement, &p, &applied) ==
            CLAY_OK);
    CHECK(applied == 5);

    // Each one alone: one item, so nothing is reused and nothing can leak.
    for (float x : xs) {
        CDoc alone;
        const clay_layer_id one = build(alone.doc, {x});
        size_t applied_one = 0;
        REQUIRE(clay_layer_move_surface(alone.doc, one, centre, displacement, &p,
                                        &applied_one) == CLAY_OK);
        REQUIRE(applied_one == 1);
        // The surface over this item must agree, whichever document it was in.
        CHECK(top_at(together.doc, x) == doctest::Approx(top_at(alone.doc, x)).epsilon(1e-4));
    }
}
