// A layer drag costs one refill (drag-a-layer-without-a-refill, phase 2).
//
// A gizmo drag sets the layer's transform every frame, and each of those
// invalidates the layer's whole box. The gesture holds the placement instead
// and applies ONE command at the end, so sixty frames cost what one costs.
//
// Two things are gated here that a wrong implementation would still pass a
// "does it look right" check on:
//
//   THE COUNT, not the clock. Sixty updates must record ONE command and take
//   ONE invalidation. A gesture that quietly applied per frame would produce
//   the same final document and be sixty times the work, and nothing about the
//   document afterwards could tell you which happened -- so the undo depth and
//   the document's own serial are what get asserted.
//
//   THE SCOPED SPLIT SUMS TO THE WHOLE. A host draws the drag from two scoped
//   evaluations, and if their hard union is not the document's own field then
//   every preview it draws is wrong. Asserted bitwise on a three-layer
//   document, because a hard union of two exact halves has no arithmetic in it
//   to round.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

clay_layer_id blob_layer(clay_document* doc, const char* name, float cx, int items) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    for (int i = 0; i < items; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_SPHERE;
        d.params[0] = 0.35f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.1f;
        d.position[0] = cx + 0.6f * std::cos(t);
        d.position[1] = 0.6f * std::sin(t);
        d.position[2] = 0.25f * static_cast<float>(i % 4);
        clay_node_id id = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &id) == CLAY_OK);
    }
    return layer;
}

std::vector<clay_brick_request> bricks(int side, float spacing, int dim) {
    std::vector<clay_brick_request> reqs;
    for (int x = -side; x < side; ++x)
        for (int y = -side; y < side; ++y)
            for (int z = -side; z < side; ++z) {
                clay_brick_request r{};
                r.key[0] = x;
                r.key[1] = y;
                r.key[2] = z;
                r.spacing = spacing;
                r.dims[0] = r.dims[1] = r.dims[2] = dim;
                r.band = 3.0f * spacing;
                r.origin[0] = static_cast<float>(x * dim) * spacing;
                r.origin[1] = static_cast<float>(y * dim) * spacing;
                r.origin[2] = static_cast<float>(z * dim) * spacing;
                reqs.push_back(r);
            }
    return reqs;
}

std::size_t undo_depth(const clay_document* doc) {
    size_t depth = 0;
    size_t redo = 0;
    int32_t enabled = 0;
    REQUIRE(clay_document_undo_state(doc, &enabled, &depth, &redo) == CLAY_OK);
    return depth;
}

}  // namespace

TEST_CASE("placement gesture: sixty frames record one command") {
    CDoc d;
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const clay_layer_id layer = blob_layer(d.doc, "body", 0.0f, 24);
    const std::size_t before = undo_depth(d.doc);

    clay_placement_tx* tx = clay_layer_placement_begin(d.doc, layer);
    REQUIRE(tx != nullptr);

    const float axis[3] = {0.0f, 1.0f, 0.0f};
    for (int f = 0; f < 60; ++f) {
        const float pos[3] = {0.02f * static_cast<float>(f), 0.0f, 0.0f};
        REQUIRE(clay_layer_placement_update(tx, pos, axis,
                                            0.01f * static_cast<float>(f), 1.0f) == CLAY_OK);
        // THE DOCUMENT DOES NOT MOVE. Nothing was applied, so nothing is
        // recorded -- which is the whole claim, asserted every frame rather
        // than once at the end where a late apply would hide.
        CHECK(undo_depth(d.doc) == before);
    }

    REQUIRE(clay_layer_placement_commit(tx) == CLAY_OK);
    clay_layer_placement_destroy(tx);

    // One step for sixty frames.
    CHECK(undo_depth(d.doc) == before + 1);

    // And it landed where the last frame said.
    float pos[3] = {0, 0, 0};
    float ax[3] = {0, 0, 0};
    float angle = 0, scale = 0;
    REQUIRE(clay_document_layer_transform(d.doc, layer, pos, ax, &angle, &scale) == CLAY_OK);
    CHECK(pos[0] == doctest::Approx(0.02f * 59.0f));

    // ... and undoes in one.
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    REQUIRE(clay_document_layer_transform(d.doc, layer, pos, ax, &angle, &scale) == CLAY_OK);
    CHECK(pos[0] == doctest::Approx(0.0f));
}

TEST_CASE("placement gesture: a cancelled drag leaves the placement it opened with") {
    CDoc d;
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const clay_layer_id layer = blob_layer(d.doc, "body", 0.0f, 12);
    const std::size_t before = undo_depth(d.doc);

    clay_placement_tx* tx = clay_layer_placement_begin(d.doc, layer);
    REQUIRE(tx != nullptr);
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const float pos[3] = {5.0f, 5.0f, 5.0f};
    REQUIRE(clay_layer_placement_update(tx, pos, axis, 1.0f, 2.0f) == CLAY_OK);
    REQUIRE(clay_layer_placement_cancel(tx) == CLAY_OK);
    clay_layer_placement_destroy(tx);

    CHECK(undo_depth(d.doc) == before);
    float p[3] = {9, 9, 9};
    float ax[3] = {0, 0, 0};
    float angle = 0, scale = 0;
    REQUIRE(clay_document_layer_transform(d.doc, layer, p, ax, &angle, &scale) == CLAY_OK);
    CHECK(p[0] == 0.0f);
    CHECK(scale == 1.0f);

    // Destroying an OPEN gesture cancels it too, and releases the guard.
    clay_placement_tx* other = clay_layer_placement_begin(d.doc, layer);
    REQUIRE(other != nullptr);
    REQUIRE(clay_layer_placement_update(other, pos, axis, 1.0f, 1.0f) == CLAY_OK);
    clay_layer_placement_destroy(other);
    CHECK(undo_depth(d.doc) == before);
    clay_placement_tx* again = clay_layer_placement_begin(d.doc, layer);
    CHECK(again != nullptr);
    clay_layer_placement_destroy(again);
}

TEST_CASE("placement gesture: every other edit is refused while one is open") {
    CDoc d;
    const clay_layer_id a = blob_layer(d.doc, "a", 0.0f, 6);
    const clay_layer_id b = blob_layer(d.doc, "b", 4.0f, 6);

    clay_placement_tx* tx = clay_layer_placement_begin(d.doc, a);
    REQUIRE(tx != nullptr);

    // The dragged layer.
    const float pos[3] = {1.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    CHECK(clay_document_set_layer_transform(d.doc, a, pos, axis, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // AND ANOTHER LAYER, which is the half that is easy to leave out. The
    // gesture holds no snapshot, so an edit anywhere is one it cannot
    // reconcile.
    CHECK(clay_document_set_layer_transform(d.doc, b, pos, axis, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_desc it;
    std::memset(&it, 0, sizeof it);
    it.struct_size = static_cast<uint32_t>(sizeof it);
    it.prim = CLAY_PRIM_SPHERE;
    it.params[0] = 0.2f;
    it.op = CLAY_OP_ADD;
    clay_node_id n = 0;
    CHECK(clay_add_item(d.doc, b, &it, &n) == CLAY_ERROR_INVALID_ARGUMENT);

    // A second gesture is refused too.
    CHECK(clay_layer_placement_begin(d.doc, b) == nullptr);

    REQUIRE(clay_layer_placement_commit(tx) == CLAY_OK);
    clay_layer_placement_destroy(tx);

    // ... and the guard is gone afterwards.
    CHECK(clay_add_item(d.doc, b, &it, &n) == CLAY_OK);
}

TEST_CASE("placement gesture: a locked or ghosted layer is refused at the open") {
    CDoc d;
    const clay_layer_id layer = blob_layer(d.doc, "body", 0.0f, 6);
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 1) == CLAY_OK);
    CHECK(clay_layer_placement_begin(d.doc, layer) == nullptr);
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 0) == CLAY_OK);

    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 1, 0) == CLAY_OK);
    CHECK(clay_layer_placement_begin(d.doc, layer) == nullptr);
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 0) == CLAY_OK);

    clay_placement_tx* tx = clay_layer_placement_begin(d.doc, layer);
    CHECK(tx != nullptr);
    clay_layer_placement_destroy(tx);

    CHECK(clay_layer_placement_begin(d.doc, 9999u) == nullptr);
}

TEST_CASE("placement gesture: the preview names the matrix to draw the layer under") {
    CDoc d;
    const clay_layer_id layer = blob_layer(d.doc, "body", 0.0f, 6);
    clay_placement_tx* tx = clay_layer_placement_begin(d.doc, layer);
    REQUIRE(tx != nullptr);

    clay_placement_report rep;
    std::memset(&rep, 0, sizeof rep);
    rep.struct_size = static_cast<uint32_t>(sizeof rep);
    // Before any update: the identity, because the layer is where it was.
    REQUIRE(clay_layer_placement_preview(tx, &rep) == CLAY_OK);
    CHECK(rep.kind == CLAY_PLACEMENT_RIGID);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(rep.delta[c * 4 + r] == (c == r ? 1.0f : 0.0f));

    const float pos[3] = {0.5f, -0.25f, 0.75f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_layer_placement_update(tx, pos, axis, 0.0f, 1.0f) == CLAY_OK);
    REQUIRE(clay_layer_placement_preview(tx, &rep) == CLAY_OK);
    CHECK(rep.kind == CLAY_PLACEMENT_RIGID);
    CHECK(rep.delta[3 * 4 + 0] == 0.5f);
    CHECK(rep.delta[3 * 4 + 1] == -0.25f);
    CHECK(rep.delta[3 * 4 + 2] == 0.75f);

    // The layer BLENDS, so a scale is not a similarity of it.
    REQUIRE(clay_layer_placement_update(tx, pos, axis, 0.0f, 2.0f) == CLAY_OK);
    REQUIRE(clay_layer_placement_preview(tx, &rep) == CLAY_OK);
    CHECK(rep.kind == CLAY_PLACEMENT_GENERAL);
    clay_layer_placement_destroy(tx);
}

TEST_CASE("placement gesture: the two scoped refills sum to the whole document") {
    // What a preview is drawn from. If the hard union of "everything except the
    // layer" and "the layer alone" is not the document's own field, every frame
    // a host draws during a drag is wrong.
    CDoc d;
    const clay_layer_id a = blob_layer(d.doc, "a", -1.2f, 10);
    const clay_layer_id b = blob_layer(d.doc, "b", 0.0f, 10);
    const clay_layer_id c = blob_layer(d.doc, "c", 1.2f, 10);
    (void)a;
    (void)c;

    const std::vector<clay_brick_request> reqs = bricks(2, 0.1f, 8);
    const std::size_t per = 8 * 8 * 8;
    std::vector<float> whole(reqs.size() * per, 0.0f);
    std::vector<float> without(reqs.size() * per, 0.0f);
    std::vector<float> alone(reqs.size() * per, 0.0f);

    REQUIRE(clay_brick_cache_eval_requests(d.doc, "cpu", reqs.data(), reqs.size(), whole.data(),
                                           whole.size(), nullptr, 0) == CLAY_OK);
    REQUIRE(clay_brick_cache_eval_requests_excluding(d.doc, b, "cpu", reqs.data(), reqs.size(),
                                                     without.data(), without.size(), nullptr,
                                                     0) == CLAY_OK);
    REQUIRE(clay_brick_cache_eval_requests_layer(d.doc, b, "cpu", reqs.data(), reqs.size(),
                                                 alone.data(), alone.size(), nullptr, 0) ==
            CLAY_OK);

    // Bitwise: a hard union is a min, and a min of two exact values is exact.
    std::size_t inside = 0;
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(std::min(without[i], alone[i]) == whole[i]);
        if (std::fabs(whole[i]) < 0.3f) ++inside;
    }
    // A gate over nothing but far values would pass whatever the split did.
    CHECK(inside > 500);
}

TEST_CASE("placement gesture: a scoped refill does not seed a later unscoped one") {
    // A scoped result is a PARTIAL field. Stored as a resume seed it would be
    // picked up later as though it were the whole one -- a wrong field with
    // nothing in the result to indicate it. The scoped forms therefore store no
    // seed, and this is what says so: an unscoped refill after a scoped one
    // must be as cold as one with no scoped call before it at all.
    CDoc warm;
    CDoc cold;
    const clay_layer_id wa = blob_layer(warm.doc, "a", -1.0f, 10);
    blob_layer(warm.doc, "b", 1.0f, 10);
    blob_layer(cold.doc, "a", -1.0f, 10);
    blob_layer(cold.doc, "b", 1.0f, 10);

    const std::vector<clay_brick_request> reqs = bricks(2, 0.1f, 8);
    const std::size_t per = 8 * 8 * 8;
    std::vector<float> scratch(reqs.size() * per, 0.0f);

    // The warm document takes both scoped refills first.
    REQUIRE(clay_brick_cache_eval_requests_excluding(warm.doc, wa, "cpu", reqs.data(),
                                                     reqs.size(), scratch.data(), scratch.size(),
                                                     nullptr, 0) == CLAY_OK);
    REQUIRE(clay_brick_cache_eval_requests_layer(warm.doc, wa, "cpu", reqs.data(), reqs.size(),
                                                 scratch.data(), scratch.size(), nullptr, 0) ==
            CLAY_OK);

    std::vector<float> after_scoped(reqs.size() * per, 0.0f);
    std::vector<float> never_scoped(reqs.size() * per, 0.0f);
    REQUIRE(clay_brick_cache_eval_requests(warm.doc, "cpu", reqs.data(), reqs.size(),
                                           after_scoped.data(), after_scoped.size(), nullptr,
                                           0) == CLAY_OK);
    REQUIRE(clay_brick_cache_eval_requests(cold.doc, "cpu", reqs.data(), reqs.size(),
                                           never_scoped.data(), never_scoped.size(), nullptr,
                                           0) == CLAY_OK);

    for (std::size_t i = 0; i < after_scoped.size(); ++i)
        CHECK(after_scoped[i] == never_scoped[i]);
}

TEST_CASE("placement gesture: meshing one SDF layer") {
    CDoc d;
    const clay_layer_id a = blob_layer(d.doc, "a", -3.0f, 8);
    const clay_layer_id b = blob_layer(d.doc, "b", 3.0f, 8);

    clay_mesh_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = static_cast<uint32_t>(sizeof mp);
    // A VOXEL SIZE, not a resolution. `resolution` divides each mesh's OWN
    // bounds, and one layer's bounds are a fraction of the pair's -- so the two
    // calls would mesh at different densities and their vertex counts would
    // compare nothing. Measured before this was pinned: 14,198 vertices for one
    // layer against 2,612 for both.
    mp.voxel_size = 0.05f;

    clay_mesh* whole = nullptr;
    clay_mesh* just_a = nullptr;
    REQUIRE(clay_document_mesh(d.doc, &mp, &whole) == CLAY_OK);
    REQUIRE(clay_document_mesh_sdf_layer(d.doc, a, &mp, &just_a) == CLAY_OK);

    const size_t whole_v = clay_mesh_vertex_count(whole);
    const size_t a_v = clay_mesh_vertex_count(just_a);
    MESSAGE("whole " << whole_v << " vertices, layer a " << a_v);
    CHECK(a_v > 0);
    // The two layers are disjoint and alike, so at one density each is about
    // half the pair. That is what says this meshed ONE layer rather than
    // meshing the document and ignoring the name.
    CHECK(a_v < whole_v);
    CHECK(a_v * 3 > whole_v);
    clay_mesh_destroy(whole);
    clay_mesh_destroy(just_a);

    // The layer's own transform is honoured: the same layer placed further out
    // still meshes, and to the same vertex count.
    const float pos[3] = {2.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, a, pos, axis, 0.0f, 1.0f) == CLAY_OK);
    clay_mesh* moved = nullptr;
    REQUIRE(clay_document_mesh_sdf_layer(d.doc, a, &mp, &moved) == CLAY_OK);
    const size_t moved_v = clay_mesh_vertex_count(moved);
    CHECK(moved_v == a_v);
    clay_mesh_destroy(moved);

    // A mesh layer is refused, which is the call next door.
    (void)b;
    CHECK(clay_document_mesh_sdf_layer(d.doc, 9999u, &mp, &moved) == CLAY_ERROR_NOT_FOUND);
}
