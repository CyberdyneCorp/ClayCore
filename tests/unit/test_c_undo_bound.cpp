#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// What an undo changed, as a region (#210, c-abi spec).
//
// A host keeping a brick cache has to turn "something was undone" into a box
// to dirty. Before these two calls the narrowest honest answer was the whole
// layer, so undoing one dab refilled the model — and the tempting alternative,
// diffing the layer's nodes across the call, is silently WRONG for an in-place
// edit, which keeps its node id. So this suite pins both halves: the bound is
// tight enough to be worth having, and loose enough never to cut a blend seam.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
        REQUIRE(clay_document_enable_undo(d) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_item_desc sphere_desc(float radius, float x, float y, float z) {
    clay_item_desc item;
    std::memset(&item, 0, sizeof item);
    item.struct_size = sizeof item;
    item.prim = CLAY_PRIM_SPHERE;
    item.params[0] = radius;
    item.position[0] = x;
    item.position[1] = y;
    item.position[2] = z;
    item.rotation[3] = 1.0f;
    item.scale = 1.0f;
    item.op = CLAY_OP_ADD;
    return item;
}

clay_node_id add_sphere(Doc& doc, float radius, float x, float y, float z) {
    clay_item_desc item = sphere_desc(radius, x, y, z);
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc.d, doc.layer, &item, &node) == CLAY_OK);
    return node;
}

// The three states, kept together so a test reads them as one answer.
struct Bound {
    std::int32_t undone = -1;
    std::int32_t has = -1;
    std::int32_t infinite = -1;
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};

    bool covers(float x, float y, float z) const {
        return has == 1 && infinite == 0 && lo[0] <= x && hi[0] >= x && lo[1] <= y && hi[1] >= y &&
               lo[2] <= z && hi[2] >= z;
    }
};

Bound undo_of(Doc& doc) {
    Bound b;
    REQUIRE(clay_document_undo_bound(doc.d, &b.undone, b.lo, b.hi, &b.has, &b.infinite) == CLAY_OK);
    return b;
}

Bound redo_of(Doc& doc) {
    Bound b;
    REQUIRE(clay_document_redo_bound(doc.d, &b.undone, b.lo, b.hi, &b.has, &b.infinite) == CLAY_OK);
    return b;
}

// The layer's own bound, which is what a host had to settle for before.
Bound layer_bound(Doc& doc) {
    Bound b;
    REQUIRE(clay_layer_influence_bound(doc.d, doc.layer, b.lo, b.hi, &b.has, &b.infinite) ==
            CLAY_OK);
    return b;
}

}  // namespace

TEST_CASE("undoing a dab bounds itself, not the layer") {
    Doc doc;
    // A model spread over x, so a bound covering it all is visibly wider than
    // a bound covering one dab of it — the #210 shape.
    for (int i = 0; i < 8; ++i) add_sphere(doc, 0.4f, static_cast<float>(i), 0.0f, 0.0f);
    Bound layer = layer_bound(doc);
    REQUIRE(layer.has == 1);

    clay_node_id dab = add_sphere(doc, 0.2f, 0.0f, 0.0f, 0.0f);
    float dlo[3], dhi[3];
    std::int32_t dhas = 0, dinf = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, dab, dlo, dhi, &dhas, &dinf) ==
            CLAY_OK);
    REQUIRE(dhas == 1);

    Bound b = undo_of(doc);
    CHECK(b.undone == 1);
    REQUIRE(b.has == 1);
    CHECK(b.infinite == 0);
    // It covers what went away...
    for (int a = 0; a < 3; ++a) {
        CHECK(b.lo[a] <= dlo[a]);
        CHECK(b.hi[a] >= dhi[a]);
    }
    // ...and stops well short of the layer, which is the whole point.
    CHECK(b.hi[0] < layer.hi[0]);

    // Redo reports the same region: the dab comes back where it was.
    Bound r = redo_of(doc);
    CHECK(r.undone == 1);
    REQUIRE(r.has == 1);
    for (int a = 0; a < 3; ++a) {
        CHECK(r.lo[a] == doctest::Approx(b.lo[a]));
        CHECK(r.hi[a] == doctest::Approx(b.hi[a]));
    }
}

TEST_CASE("a move is bounded at both ends") {
    Doc doc;
    clay_node_id id = add_sphere(doc, 0.3f, 0.0f, 0.0f, 0.0f);
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const float far_away[3] = {5.0f, 0.0f, 0.0f};
    REQUIRE(clay_layer_set_transform(doc.d, doc.layer, id, far_away, axis, 0.0f, 1.0f) == CLAY_OK);

    Bound b = undo_of(doc);
    CHECK(b.undone == 1);
    // Where it was moved to, and where it went back to. A bound taken on one
    // side of the apply would hold one of these and not the other, and the
    // half it missed is where the stale bricks would be.
    CHECK(b.covers(5.0f, 0.0f, 0.0f));
    CHECK(b.covers(0.0f, 0.0f, 0.0f));
}

TEST_CASE("an undone removal is bounded by what came back") {
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    clay_node_id gone = add_sphere(doc, 0.3f, 3.0f, 0.0f, 0.0f);
    REQUIRE(clay_remove_node(doc.d, doc.layer, gone) == CLAY_OK);

    // Undo restores it: the region to dirty is where it reappeared.
    Bound b = undo_of(doc);
    CHECK(b.undone == 1);
    CHECK(b.covers(3.0f, 0.0f, 0.0f));

    // Redo removes it again, and reports the same region — the hole it left.
    Bound r = redo_of(doc);
    CHECK(r.undone == 1);
    CHECK(r.covers(3.0f, 0.0f, 0.0f));
}

TEST_CASE("a child of a blended group covers the seam without covering the group") {
    Doc doc;
    const float k = 0.5f;
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, k,
                                 0.0f, &group) == CLAY_OK);
    clay_item_desc anchor = sphere_desc(0.3f, 0.0f, 0.0f, 0.0f);
    clay_node_id anchor_id = 0;
    REQUIRE(clay_add_item_in_group(doc.d, doc.layer, group, -1, &anchor, &anchor_id) == CLAY_OK);

    clay_item_desc child = sphere_desc(0.3f, 2.0f, 0.0f, 0.0f);
    clay_node_id child_id = 0;
    REQUIRE(clay_add_item_in_group(doc.d, doc.layer, group, -1, &child, &child_id) == CLAY_OK);

    float clo[3], chi[3];
    std::int32_t chas = 0, cinf = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, child_id, clo, chi, &chas, &cinf) ==
            CLAY_OK);
    REQUIRE(chas == 1);
    float glo[3], ghi[3];
    std::int32_t ghas = 0, ginf = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, group, glo, ghi, &ghas, &ginf) ==
            CLAY_OK);
    REQUIRE(ghas == 1);

    Bound b = undo_of(doc);  // undo the child's add
    CHECK(b.undone == 1);
    REQUIRE(b.has == 1);
    // BOTH halves, and the change to this case is the second one.
    //
    // The group's blend spreads the child's influence past the child's own
    // box, so a bound that stopped at the child would leave a stale seam. That
    // has always been the requirement and still is.
    CHECK(b.hi[0] > chi[0]);
    // It used to be met by reporting the GROUP's whole bound, which also
    // covers the anchor at the far end and everything between. The anchor is
    // not something an edit to the child can reach, and including it made the
    // region grow with the size of the group rather than with the size of the
    // edit. The bound is now the child's, dilated by the group's blend
    // support: still past the seam, and strictly inside the group.
    CHECK(b.lo[0] > glo[0]);
    CHECK_FALSE(b.lo[0] <= glo[0]);
    // The seam it must cover is the one at the child's own edge, and the
    // dilation that covers it is the group's support -- which for a quadratic
    // profile is wider than k, so asserting `>= k` here is the weaker claim
    // that holds for every profile.
    CHECK(b.hi[0] >= chi[0] + k);
    // Never SMALLER than the child's own influence: a bound that is may leave
    // stale bricks, which is the failure this whole family of checks exists
    // to prevent.
    for (int a = 0; a < 3; ++a) {
        CHECK(b.lo[a] <= clo[a]);
        CHECK(b.hi[a] >= chi[a]);
    }
}

TEST_CASE("an unbounded node reports unbounded, not a box") {
    Doc doc;
    add_sphere(doc, 0.5f, 0.0f, 0.0f, 0.0f);
    // An UNBOUNDED PRIMITIVE: one of the things whose influence is still
    // genuinely infinite. This was an intersect until #319 gave that one the
    // layer's extent, and an intersect here would exercise the finite path
    // under an infinite name. A plane rather than a spatial morph only because
    // clay_item_desc cannot express a transition's span — the two are the same
    // Nonlocality::Unbounded either way.
    clay_item_desc cut;
    std::memset(&cut, 0, sizeof cut);
    cut.struct_size = sizeof cut;
    cut.prim = CLAY_PRIM_PLANE;
    cut.params[0] = 0.0f;
    cut.params[1] = 1.0f;
    cut.params[2] = 0.0f;
    cut.params[3] = 0.0f;
    cut.rotation[3] = 1.0f;
    cut.scale = 1.0f;
    cut.op = CLAY_OP_ADD;
    clay_node_id id = 0;
    REQUIRE(clay_add_item(doc.d, doc.layer, &cut, &id) == CLAY_OK);

    Bound b;
    b.lo[0] = b.hi[0] = 1234.5f;
    REQUIRE(clay_document_undo_bound(doc.d, &b.undone, b.lo, b.hi, &b.has, &b.infinite) == CLAY_OK);
    CHECK(b.undone == 1);
    CHECK(b.has == 1);
    CHECK(b.infinite == 1);
    // An unbounded answer writes no box, exactly as the influence-bound
    // queries do — the host's response is mark_dirty with both regions NULL.
    CHECK(b.lo[0] == 1234.5f);
    CHECK(b.hi[0] == 1234.5f);
}

TEST_CASE("undoing an INTERSECT reports the layer's box, not unbounded") {
    // The other half of the split (#319). Undo takes the same influence bound
    // every other consumer does, so the finite answer has to reach it too —
    // and a host that dirties the whole cache on every intersect undo is the
    // cost this removes.
    Doc doc;
    add_sphere(doc, 0.5f, 0.0f, 0.0f, 0.0f);
    clay_item_desc cut = sphere_desc(0.3f, 0.2f, 0.0f, 0.0f);
    cut.op = CLAY_OP_INTERSECT;
    clay_node_id id = 0;
    REQUIRE(clay_add_item(doc.d, doc.layer, &cut, &id) == CLAY_OK);

    Bound b;
    REQUIRE(clay_document_undo_bound(doc.d, &b.undone, b.lo, b.hi, &b.has, &b.infinite) == CLAY_OK);
    CHECK(b.undone == 1);
    CHECK(b.has == 1);
    CHECK(b.infinite == 0);
    CHECK(b.lo[0] <= -0.5f);  // the layer's extent, which holds the 0.5 sphere
    CHECK(b.hi[0] >= 0.5f);
}

TEST_CASE("an edit that cannot change the field dirties nothing") {
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    REQUIRE(clay_document_set_layer_name(doc.d, doc.layer, "torso") == CLAY_OK);

    Bound b = undo_of(doc);
    CHECK(b.undone == 1);  // the rename WAS undone...
    CHECK(b.has == 0);     // ...and there is nothing to re-evaluate for it
    CHECK(b.infinite == 0);
}

TEST_CASE("nothing to undo reports nothing to dirty, and is not an error") {
    Doc doc;
    Bound b = undo_of(doc);
    CHECK(b.undone == 0);
    CHECK(b.has == 0);
    CHECK(b.infinite == 0);

    Bound r = redo_of(doc);
    CHECK(r.undone == 0);
    CHECK(r.has == 0);
}

TEST_CASE("the reporting pair agrees with the plain pair") {
    // Same edits, same sequence, one document driven through each call: the
    // bound is extra information and never a different undo.
    auto build = [](Doc& doc) {
        add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
        clay_node_id id = add_sphere(doc, 0.3f, 1.0f, 0.0f, 0.0f);
        const float axis[3] = {0, 1, 0};
        const float p[3] = {2.0f, 0.0f, 0.0f};
        REQUIRE(clay_layer_set_transform(doc.d, doc.layer, id, p, axis, 0.0f, 1.0f) == CLAY_OK);
    };
    Doc plain;
    Doc reporting;
    build(plain);
    build(reporting);

    for (int step = 0; step < 3; ++step) {
        std::int32_t undone = -1;
        REQUIRE(clay_document_undo(plain.d, &undone) == CLAY_OK);
        Bound b = undo_of(reporting);
        CHECK(b.undone == undone);
    }
    // A fourth on an empty stack, then compare what the two documents hold.
    std::int32_t undone = -1;
    REQUIRE(clay_document_undo(plain.d, &undone) == CLAY_OK);
    CHECK(undone == 0);
    CHECK(undo_of(reporting).undone == 0);

    size_t plain_count = 0, reporting_count = 0;
    REQUIRE(clay_layer_node_count(plain.d, plain.layer, &plain_count) == CLAY_OK);
    REQUIRE(clay_layer_node_count(reporting.d, reporting.layer, &reporting_count) == CLAY_OK);
    CHECK(plain_count == reporting_count);
    CHECK(plain_count == 0);
}

TEST_CASE("the refusals are the plain pair's refusals") {
    float lo[3], hi[3];
    std::int32_t undone = 0, has = 0, infinite = 0;
    clay_document* off = clay_document_create();
    REQUIRE(off != nullptr);
    // Undo not enabled, and a null out-count, are refused as they are today.
    CHECK(clay_document_undo_bound(off, &undone, lo, hi, &has, &infinite) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_redo_bound(off, &undone, lo, hi, &has, &infinite) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_enable_undo(off) == CLAY_OK);
    CHECK(clay_document_undo_bound(off, nullptr, lo, hi, &has, &infinite) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_undo_bound(nullptr, &undone, lo, hi, &has, &infinite) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // Every bound out-pointer is optional — the call is then exactly undo.
    CHECK(clay_document_undo_bound(off, &undone, nullptr, nullptr, nullptr, nullptr) == CLAY_OK);
    clay_document_destroy(off);
}
