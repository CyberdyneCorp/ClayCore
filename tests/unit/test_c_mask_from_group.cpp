// A group becomes a mask, so a named region is a SELECTION (c-abi spec).
//
// The two lattices already interoperated one way: paint a mask however you
// like and name the result a group (`clay_groups_fill_from_mask`). Without the
// reverse, a group could gate a brush only through CLAY_AUTOMASK_SURFACE_GROUP
// -- "stay in the one I started in" -- and never through a mask a caller CHOSE.
// "Flatten this whole panel" was expressible only as a stroke that happens to
// begin on it, which is a different operation: an artist selects a region and
// THEN decides what to do to it.
//
// WHAT THIS FILE HAS TO SHOW, and only the first is about the plumbing:
//
//   1. The round trip is the same region. group -> mask -> group must land on
//      the cells it started on, or the two directions describe different
//      borders and a host cannot trust either.
//   2. It agrees with `clay_groups_at` point by point, which is the statement
//      a caller actually relies on when it masks a region and sculpts.
//   3. Zero ERASES rather than writing zeros, because that is what zero means
//      everywhere else in the mask API.
//   4. The two cell sizes need not match.

#include <doctest/doctest.h>

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

clay_layer_id ball(clay_document* doc) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<uint32_t>(sizeof d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = 1.0f;
    d.op = CLAY_OP_ADD;
    clay_node_id n = 0;
    REQUIRE(clay_add_item(doc, layer, &d, &n) == CLAY_OK);
    return layer;
}

clay_groups* groups_of(clay_document* doc, float cell) {
    clay_groups* g = nullptr;
    REQUIRE(clay_document_groups(doc, cell, &g) == CLAY_OK);
    REQUIRE(g != nullptr);
    return g;
}

// A named region: the +x half of the ball.
void name_half(clay_groups* g, uint16_t id) {
    const float lo[3] = {0.0f, -1.4f, -1.4f};
    const float hi[3] = {1.4f, 1.4f, 1.4f};
    REQUIRE(clay_groups_fill(g, lo, hi, id) == CLAY_OK);
}

std::vector<float> probe_points() {
    std::vector<float> p;
    for (int i = 0; i < 11; ++i)
        for (int j = 0; j < 11; ++j)
            for (int k = 0; k < 11; ++k) {
                p.push_back(-1.2f + 0.24f * static_cast<float>(i));
                p.push_back(-1.2f + 0.24f * static_cast<float>(j));
                p.push_back(-1.2f + 0.24f * static_cast<float>(k));
            }
    return p;
}

}  // namespace

TEST_CASE("mask from group: the round trip is the same region") {
    // THE LOAD-BEARING GATE. If group -> mask -> group does not land on the
    // cells it started on, the two directions describe different borders and a
    // host cannot trust either of them.
    CDoc d;
    ball(d.doc);
    clay_groups* g = groups_of(d.doc, 0.1f);
    name_half(g, 7);

    uint64_t before = 0;
    REQUIRE(clay_groups_cell_count(g, 7, &before) == CLAY_OK);
    REQUIRE(before > 0);

    clay_mask* mask = clay_mask_create(0.1f);
    REQUIRE(mask != nullptr);
    uint64_t painted = 0;
    REQUIRE(clay_mask_fill_from_group(mask, g, 7, 1.0f, &painted) == CLAY_OK);
    MESSAGE("group held " << before << " cells, mask painted " << painted);
    CHECK(painted > 0);

    // Back again, into a second id on a fresh lattice.
    CDoc e;
    ball(e.doc);
    clay_groups* g2 = groups_of(e.doc, 0.1f);
    uint64_t claimed = 0;
    REQUIRE(clay_groups_fill_from_mask(g2, mask, 7, 0.5f, &claimed) == CLAY_OK);
    uint64_t after = 0;
    REQUIRE(clay_groups_cell_count(g2, 7, &after) == CLAY_OK);
    MESSAGE("round trip: " << before << " -> " << after);
    CHECK(after == before);

    // ... and cell for cell, not merely by count.
    const std::vector<float> pts = probe_points();
    std::size_t inside = 0;
    for (std::size_t i = 0; i < pts.size(); i += 3) {
        uint16_t a = 0, b = 0;
        REQUIRE(clay_groups_at(g, &pts[i], &a) == CLAY_OK);
        REQUIRE(clay_groups_at(g2, &pts[i], &b) == CLAY_OK);
        CHECK(a == b);
        if (a == 7) ++inside;
    }
    // A gate over nothing would pass.
    CHECK(inside > 100);

    clay_mask_destroy(mask);
}

TEST_CASE("mask from group: the mask agrees with the group point by point") {
    // What a caller actually relies on: mask the region, then sculpt, and the
    // gate is where the group said it was.
    CDoc d;
    ball(d.doc);
    clay_groups* g = groups_of(d.doc, 0.1f);
    name_half(g, 3);

    clay_mask* mask = clay_mask_create(0.1f);
    REQUIRE(mask != nullptr);
    REQUIRE(clay_mask_fill_from_group(mask, g, 3, 1.0f, nullptr) == CLAY_OK);

    const std::vector<float> pts = probe_points();
    std::vector<float> values(pts.size() / 3, 0.0f);
    REQUIRE(clay_mask_sample_many(mask, pts.data(), values.size(), values.data()) == CLAY_OK);

    std::size_t in = 0, out = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        uint16_t id = 0;
        REQUIRE(clay_groups_at(g, &pts[i * 3], &id) == CLAY_OK);
        if (id == 3) {
            CHECK(values[i] == 1.0f);
            ++in;
        } else {
            CHECK(values[i] == 0.0f);
            ++out;
        }
    }
    MESSAGE("agreed on " << in << " inside and " << out << " outside");
    CHECK(in > 100);
    CHECK(out > 100);
    clay_mask_destroy(mask);
}

TEST_CASE("mask from group: zero erases rather than writing zeros") {
    // Zero is what releases a cell's storage everywhere else in this API, so a
    // group must be able to UN-mask its own region rather than leave explicit
    // zeros behind. A mask full of zeros and an empty mask gate identically but
    // do not cost the same, and `painted_count` is what says which you have.
    CDoc d;
    ball(d.doc);
    clay_groups* g = groups_of(d.doc, 0.1f);
    name_half(g, 5);

    clay_mask* mask = clay_mask_create(0.1f);
    REQUIRE(mask != nullptr);
    // Paint everything, then erase just the group.
    const float lo[3] = {-1.4f, -1.4f, -1.4f};
    const float hi[3] = {1.4f, 1.4f, 1.4f};
    REQUIRE(clay_mask_fill(mask, lo, hi, 1.0f) == CLAY_OK);
    uint64_t all = 0;
    REQUIRE(clay_mask_painted_count(mask, &all) == CLAY_OK);
    REQUIRE(all > 0);

    uint64_t erased = 0;
    REQUIRE(clay_mask_fill_from_group(mask, g, 5, 0.0f, &erased) == CLAY_OK);
    CHECK(erased > 0);
    uint64_t left = 0;
    REQUIRE(clay_mask_painted_count(mask, &left) == CLAY_OK);
    MESSAGE("painted " << all << ", erased " << erased << ", left " << left);
    // Storage RELEASED, not overwritten with zeros.
    CHECK(left == all - erased);

    // And the erased cells really read zero.
    const std::vector<float> pts = probe_points();
    std::vector<float> values(pts.size() / 3, 0.0f);
    REQUIRE(clay_mask_sample_many(mask, pts.data(), values.size(), values.data()) == CLAY_OK);
    for (std::size_t i = 0; i < values.size(); ++i) {
        uint16_t id = 0;
        REQUIRE(clay_groups_at(g, &pts[i * 3], &id) == CLAY_OK);
        if (id == 5) CHECK(values[i] == 0.0f);
    }
    clay_mask_destroy(mask);
}

TEST_CASE("mask from group: the two cell sizes need not match") {
    // A fine mask over a coarse group quantises to the GROUP, which is the
    // border every other group operation draws. The alternative -- refusing
    // unless they match -- would make a host keep two lattices in step for no
    // gain, and clay_groups_fill_from_mask already accepts the mismatch in the
    // other direction.
    CDoc d;
    ball(d.doc);
    clay_groups* g = groups_of(d.doc, 0.2f);  // coarse
    name_half(g, 9);

    clay_mask* fine = clay_mask_create(0.05f);  // four times finer
    REQUIRE(fine != nullptr);
    uint64_t painted = 0;
    REQUIRE(clay_mask_fill_from_group(fine, g, 9, 1.0f, &painted) == CLAY_OK);
    CHECK(painted > 0);

    const std::vector<float> pts = probe_points();
    std::vector<float> values(pts.size() / 3, 0.0f);
    REQUIRE(clay_mask_sample_many(fine, pts.data(), values.size(), values.data()) == CLAY_OK);
    std::size_t checked = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        uint16_t id = 0;
        REQUIRE(clay_groups_at(g, &pts[i * 3], &id) == CLAY_OK);
        CHECK(values[i] == (id == 9 ? 1.0f : 0.0f));
        ++checked;
    }
    CHECK(checked > 1000);
    clay_mask_destroy(fine);
}

TEST_CASE("mask from group: naming no group paints nothing") {
    // CLAY_NO_GROUP means "not in a group", which is not a region. Painting the
    // complement of every group is a different request and a caller that wants
    // it can invert.
    CDoc d;
    ball(d.doc);
    clay_groups* g = groups_of(d.doc, 0.1f);
    name_half(g, 2);

    clay_mask* mask = clay_mask_create(0.1f);
    REQUIRE(mask != nullptr);
    uint64_t painted = 1;
    REQUIRE(clay_mask_fill_from_group(mask, g, CLAY_NO_GROUP, 1.0f, &painted) == CLAY_OK);
    CHECK(painted == 0);
    uint64_t count = 0;
    REQUIRE(clay_mask_painted_count(mask, &count) == CLAY_OK);
    CHECK(count == 0);

    // A group that was never named paints nothing either, and is not an error.
    REQUIRE(clay_mask_fill_from_group(mask, g, 999, 1.0f, &painted) == CLAY_OK);
    CHECK(painted == 0);

    CHECK(clay_mask_fill_from_group(mask, nullptr, 2, 1.0f, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_mask_destroy(mask);
}
