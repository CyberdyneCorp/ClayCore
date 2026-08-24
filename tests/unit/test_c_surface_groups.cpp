#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay.h"

// Surface groups across the ABI (c-abi spec: add-surface-groups).
//
// The capability existed in C++ with tests and was reachable from no host at
// all: no C entry point, no pyclay, no serialisation, and hiding a group hid
// nothing because the mesher never asked. These are the tests for the surface a
// host actually calls.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;
    clay_groups* g = nullptr;

    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
        float r = 0.5f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, sdf, it, &id) == CLAY_OK);
        clay_item_destroy(it);
        REQUIRE(clay_document_groups(d, 0.05f, &g) == CLAY_OK);
    }
    ~Doc() {
        clay_groups_destroy(g);
        clay_document_destroy(d);
    }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    void name_halves() {
        const float top_lo[3] = {-0.6f, 0.0f, -0.6f}, top_hi[3] = {0.6f, 0.6f, 0.6f};
        const float bot_lo[3] = {-0.6f, -0.6f, -0.6f}, bot_hi[3] = {0.6f, -0.001f, 0.6f};
        REQUIRE(clay_groups_fill(g, top_lo, top_hi, 1) == CLAY_OK);
        REQUIRE(clay_groups_fill(g, bot_lo, bot_hi, 2) == CLAY_OK);
    }
    std::size_t mesh_triangles() {
        clay_mesh_params p{};
        p.struct_size = sizeof(p);
        p.voxel_size = 0.02f;
        clay_mesh* m = nullptr;
        if (clay_document_mesh(d, &p, &m) != CLAY_OK) return 0;
        const size_t tris = clay_mesh_index_count(m) / 3;
        clay_mesh_destroy(m);
        return tris;
    }
};

}  // namespace

TEST_CASE("c abi: a document has no groups until one is asked for") {
    clay_document* d = clay_document_create();
    int32_t has = 1;
    CHECK(clay_document_has_groups(d, &has) == CLAY_OK);
    CHECK(has == 0);

    clay_groups* g = nullptr;
    CHECK(clay_document_groups(d, 0.05f, &g) == CLAY_OK);
    // Created but EMPTY is still "no groups": a lattice nobody has written to
    // names no region, and a host building a UI from this must not show one.
    CHECK(clay_document_has_groups(d, &has) == CLAY_OK);
    CHECK(has == 0);
    clay_groups_destroy(g);
    clay_document_destroy(d);
}

TEST_CASE("c abi: a point resolves to its group and not to another") {
    Doc doc;
    doc.name_halves();
    const float top[3] = {0.0f, 0.3f, 0.0f};
    const float bottom[3] = {0.0f, -0.3f, 0.0f};
    uint16_t id = 0;
    CHECK(clay_groups_at(doc.g, top, &id) == CLAY_OK);
    CHECK(id == 1);
    CHECK(clay_groups_at(doc.g, bottom, &id) == CLAY_OK);
    CHECK(id == 2);

    // Somewhere neither fill reached.
    const float away[3] = {5.0f, 5.0f, 5.0f};
    CHECK(clay_groups_at(doc.g, away, &id) == CLAY_OK);
    CHECK(id == CLAY_NO_GROUP);
}

TEST_CASE("c abi: hiding a group removes its triangles, and showing restores them") {
    // The regression for the gap this change closes: hiding used to be a flag
    // nothing consulted, so this count never moved.
    Doc doc;
    doc.name_halves();
    const std::size_t all = doc.mesh_triangles();
    REQUIRE(all > 1000);  // non-degenerate: there is a mesh to filter

    REQUIRE(clay_groups_isolate(doc.g, 1) == CLAY_OK);
    const std::size_t isolated = doc.mesh_triangles();
    CHECK(isolated > 0);
    CHECK(isolated < all);

    // HIDING IS NOT DELETING: the field was untouched, so this is exact.
    REQUIRE(clay_groups_show_all(doc.g) == CLAY_OK);
    CHECK(doc.mesh_triangles() == all);
}

TEST_CASE("c abi: a quad export keeps its quads through the filter") {
    // mesh_data.h makes it a rule that rewriting `indices` clears `quads`, so a
    // triangle-wise filter would return a quad export with no quads.
    Doc doc;
    doc.name_halves();
    REQUIRE(clay_groups_isolate(doc.g, 1) == CLAY_OK);

    clay_quad_params p{};
    p.struct_size = sizeof(p);
    p.cell_size = 0.04f;
    clay_mesh* m = nullptr;
    REQUIRE(clay_document_mesh_quads(doc.d, &p, &m) == CLAY_OK);
    CHECK(clay_mesh_quad_count(m) > 0);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: picking steps over hidden surface rather than missing") {
    // Hiding the front of a head is how an artist reaches the INSIDE of it, so
    // a ray that stopped at hidden surface would defeat the feature. It must
    // land on what is behind instead.
    Doc doc;
    // Name the near hemisphere so a ray down -Z meets hidden surface first.
    const float near_lo[3] = {-0.6f, -0.6f, 0.0f}, near_hi[3] = {0.6f, 0.6f, 0.6f};
    REQUIRE(clay_groups_fill(doc.g, near_lo, near_hi, 1) == CLAY_OK);

    const float origin[3] = {0.0f, 0.0f, 3.0f};
    const float dir[3] = {0.0f, 0.0f, -1.0f};
    int32_t hit = 0;
    float t = 0.0f, pos[3] = {0, 0, 0};
    REQUIRE(clay_raycast(doc.d, origin, dir, &hit, &t, pos, nullptr) == CLAY_OK);
    REQUIRE(hit != 0);
    const float front_z = pos[2];
    REQUIRE(front_z > 0.0f);  // it really hit the near side first

    REQUIRE(clay_groups_set_visible(doc.g, 1, 0) == CLAY_OK);
    REQUIRE(clay_raycast(doc.d, origin, dir, &hit, &t, pos, nullptr) == CLAY_OK);
    CHECK(hit != 0);          // still a hit, not a miss
    CHECK(pos[2] < front_z);  // and it is behind where the near surface was
}

TEST_CASE("c abi: growing claims ungrouped cells and not a neighbour") {
    Doc doc;
    doc.name_halves();
    uint64_t before_1 = 0, before_2 = 0;
    REQUIRE(clay_groups_cell_count(doc.g, 1, &before_1) == CLAY_OK);
    REQUIRE(clay_groups_cell_count(doc.g, 2, &before_2) == CLAY_OK);
    REQUIRE(before_1 > 100);
    REQUIRE(before_2 > 100);

    uint64_t claimed = 0;
    REQUIRE(clay_groups_grow(doc.g, 1, 1, &claimed) == CLAY_OK);
    CHECK(claimed > 0);
    uint64_t after_2 = 0;
    REQUIRE(clay_groups_cell_count(doc.g, 2, &after_2) == CLAY_OK);
    CHECK(after_2 == before_2);  // untouched, though face-adjacent throughout
}

TEST_CASE("c abi: the border and id lists answer a count before a buffer") {
    Doc doc;
    doc.name_halves();
    size_t count = 0;
    REQUIRE(clay_groups_border(doc.g, 1, nullptr, 0, &count) == CLAY_OK);
    REQUIRE(count > 0);

    std::vector<int32_t> cells(count * 3, 0);
    size_t got = 0;
    REQUIRE(clay_groups_border(doc.g, 1, cells.data(), count, &got) == CLAY_OK);
    CHECK(got == count);

    size_t ids_count = 0;
    REQUIRE(clay_groups_ids(doc.g, nullptr, 0, &ids_count) == CLAY_OK);
    CHECK(ids_count == 2);
    std::vector<uint16_t> ids(ids_count, 0);
    REQUIRE(clay_groups_ids(doc.g, ids.data(), ids_count, &ids_count) == CLAY_OK);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
}

TEST_CASE("c abi: hiding is one undo step and reverses exactly") {
    Doc doc;
    doc.name_halves();
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    const std::size_t all = doc.mesh_triangles();

    REQUIRE(clay_groups_isolate(doc.g, 1) == CLAY_OK);
    std::size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(doc.d, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 1);
    REQUIRE(doc.mesh_triangles() < all);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
    CHECK(undone != 0);
    CHECK(doc.mesh_triangles() == all);
}

TEST_CASE("c abi: an edit that changed nothing is not an undo step") {
    Doc doc;
    doc.name_halves();
    REQUIRE(clay_groups_isolate(doc.g, 1) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);

    REQUIRE(clay_groups_isolate(doc.g, 1) == CLAY_OK);  // again
    std::size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(doc.d, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 0);
}

TEST_CASE("c abi: naming a region from a mask") {
    // How "addressed by group or by mask" stays ONE mechanism: paint a mask
    // however you like and name the result, rather than a second visibility
    // system that can disagree with the first.
    Doc doc;
    clay_mask* m = nullptr;
    REQUIRE(clay_document_add_mask(doc.d, doc.sdf, 0.05f, &m) == CLAY_OK);
    const float lo[3] = {-0.3f, -0.3f, -0.3f}, hi[3] = {0.3f, 0.3f, 0.3f};
    REQUIRE(clay_mask_fill(m, lo, hi, 1.0f) == CLAY_OK);
    size_t painted = 0;
    REQUIRE(clay_mask_painted_count(m, &painted) == CLAY_OK);
    REQUIRE(painted > 100);  // non-degenerate: the mask really holds cells

    uint64_t claimed = 0;
    REQUIRE(clay_groups_fill_from_mask(doc.g, m, 7, 0.5f, &claimed) == CLAY_OK);
    CHECK(claimed > 0);
    const float inside[3] = {0.0f, 0.0f, 0.0f};
    uint16_t id = 0;
    CHECK(clay_groups_at(doc.g, inside, &id) == CLAY_OK);
    CHECK(id == 7);
}

TEST_CASE("c abi: merging a group away takes its hidden flag with it") {
    // Or a hidden id nobody carries keeps hiding a group that no longer exists.
    Doc doc;
    doc.name_halves();
    REQUIRE(clay_groups_set_visible(doc.g, 2, 0) == CLAY_OK);
    int32_t any = 0;
    REQUIRE(clay_groups_any_hidden(doc.g, &any) == CLAY_OK);
    REQUIRE(any != 0);

    uint64_t moved = 0;
    REQUIRE(clay_groups_reassign(doc.g, 2, CLAY_NO_GROUP, &moved) == CLAY_OK);
    CHECK(moved > 0);
    REQUIRE(clay_groups_any_hidden(doc.g, &any) == CLAY_OK);
    CHECK(any == 0);
}
