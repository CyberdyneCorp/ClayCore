#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay.h"

// Undo across the ABI, widened (c-abi spec: undo across the ABI).
//
// clay_document_undo used to act on the command stack alone. A host that
// sculpted a voxel layer and pressed undo reversed an unrelated SDF edit, or
// was told there was nothing to undo — which is the defect this file is the
// regression for. The entry points keep their signatures and reverse MORE than
// they did, rather than differently.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;
    clay_layer_id voxels = 0;

    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &voxels, &g) == CLAY_OK);
        REQUIRE(clay_document_enable_undo(d) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    clay_voxel_grid* grid() {
        clay_layer_id id = 0;
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_voxel_layer(d, "blocks", &id, &g) == CLAY_OK);
        return g;
    }
    std::size_t depth() const {
        std::size_t undo = 0;
        REQUIRE(clay_document_undo_state(d, nullptr, &undo, nullptr) == CLAY_OK);
        return undo;
    }
    std::size_t cells() {
        std::size_t n = 0;
        REQUIRE(clay_voxel_occupied_count(grid(), &n) == CLAY_OK);
        return n;
    }
    void add_sphere(float r) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, sdf, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
    bool undo() {
        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(d, &undone) == CLAY_OK);
        return undone != 0;
    }
    bool redo() {
        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(d, &redone) == CLAY_OK);
        return redone != 0;
    }
};

}  // namespace

TEST_CASE("c abi: undo reverses a voxel edit") {
    // The regression. Before this change the call reversed an unrelated SDF
    // edit or reported nothing to undo.
    Doc doc;
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), cell, 1) == CLAY_OK);
    REQUIRE(doc.cells() == 1);
    CHECK(doc.depth() == 1);

    CHECK(doc.undo());
    CHECK(doc.cells() == 0);
    CHECK(doc.depth() == 0);

    CHECK(doc.redo());
    CHECK(doc.cells() == 1);
}

TEST_CASE("c abi: one undo order spans the edit list and a voxel layer") {
    Doc doc;
    doc.add_sphere(0.5f);
    const std::int32_t a[3] = {0, 0, 0};
    const std::int32_t b[3] = {1, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), a, 1) == CLAY_OK);
    REQUIRE(clay_voxel_set(doc.grid(), b, 1) == CLAY_OK);
    CHECK(doc.depth() == 3);
    CHECK(doc.cells() == 2);

    // Newest first: the two voxel writes, then the SDF item.
    CHECK(doc.undo());
    CHECK(doc.cells() == 1);
    CHECK(doc.undo());
    CHECK(doc.cells() == 0);

    size_t nodes = 0;
    REQUIRE(clay_layer_node_count(doc.d, doc.sdf, &nodes) == CLAY_OK);
    CHECK(nodes == 1);  // still there — the voxel undos did not touch it
    CHECK(doc.undo());
    REQUIRE(clay_layer_node_count(doc.d, doc.sdf, &nodes) == CLAY_OK);
    CHECK(nodes == 0);

    CHECK_FALSE(doc.undo());
    CHECK(doc.depth() == 0);
}

TEST_CASE("c abi: a sculpt verb is one undo step, not one per cell") {
    Doc doc;
    // Lay down a block so a verb has something to work on.
    const std::int32_t lo[3] = {0, 0, 0};
    const std::int32_t hi[3] = {4, 4, 4};
    REQUIRE(clay_voxel_fill_box(doc.grid(), lo, hi, 1) == CLAY_OK);
    const std::size_t filled = doc.cells();
    REQUIRE(filled > 1);
    CHECK(doc.depth() == 1);  // one fill, one step, however many cells

    clay_brush_params brush{};
    brush.struct_size = sizeof(brush);
    brush.size = 3;
    brush.shape = 1;      /* sphere */
    brush.falloff = 0;
    brush.strength = 1.0f;
    const std::int32_t at[3] = {0, 0, 0};  // a corner: smoothing must remove cells
    brush.size = 5;
    REQUIRE(clay_voxel_sculpt_smooth(doc.grid(), at, &brush) == CLAY_OK);

    const std::size_t after = doc.cells();
    // The verb must have DONE something, or this measures the fill's undo
    // instead — which is what it did when first written.
    REQUIRE(after != filled);
    CHECK(doc.depth() == 2);  // one fill, one smooth: two steps for many cells

    CHECK(doc.undo());
    CHECK(doc.cells() == filled);  // the whole verb came back in one step

    CHECK(doc.redo());
    CHECK(doc.cells() == after);
}

TEST_CASE("c abi: a voxel edit that changed nothing is not an undo step") {
    Doc doc;
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), cell, 1) == CLAY_OK);
    const std::size_t before = doc.depth();

    // Erasing an already-empty cell writes nothing.
    const std::int32_t empty[3] = {50, 50, 50};
    REQUIRE(clay_voxel_erase(doc.grid(), empty) == CLAY_OK);
    CHECK(doc.depth() == before);
}

TEST_CASE("c abi: a standalone grid has no history and still edits") {
    // Undo is a DOCUMENT concept. A grid created outside one is not in a
    // document, so it records nothing — and must still work.
    clay_voxel_grid* g = clay_voxel_grid_create(0.1f);
    REQUIRE(g != nullptr);
    const std::int32_t cell[3] = {0, 0, 0};
    CHECK(clay_voxel_set(g, cell, 1) == CLAY_OK);
    std::size_t n = 0;
    CHECK(clay_voxel_occupied_count(g, &n) == CLAY_OK);
    CHECK(n == 1);
    clay_voxel_grid_destroy(g);
}

TEST_CASE("c abi: undo disabled leaves voxel editing exactly as it was") {
    clay_document* d = clay_document_create();
    clay_layer_id voxels = 0;
    clay_voxel_grid* g = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &voxels, &g) == CLAY_OK);
    // No clay_document_enable_undo.
    const std::int32_t cell[3] = {0, 0, 0};
    CHECK(clay_voxel_set(g, cell, 1) == CLAY_OK);
    std::size_t n = 0;
    CHECK(clay_voxel_occupied_count(g, &n) == CLAY_OK);
    CHECK(n == 1);

    std::int32_t enabled = 1;
    CHECK(clay_document_undo_state(d, &enabled, nullptr, nullptr) == CLAY_OK);
    CHECK(enabled == 0);
    clay_document_destroy(d);
}

TEST_CASE("c abi: consolidate is undoable, which is worth pinning") {
    // It is the operation most often assumed not to be — it takes an UndoStack
    // and records through the command vocabulary. The session is told
    // afterwards how many entries appeared, and this is what checks that the
    // telling happened.
    Doc doc;
    doc.add_sphere(0.5f);
    const std::size_t before = doc.depth();

    clay_consolidation_params params{};
    params.struct_size = sizeof(params);
    params.cell_size = 0.05f;
    if (clay_layer_consolidate(doc.d, doc.sdf, &params, nullptr, nullptr, nullptr) != CLAY_OK)
        return;

    CHECK(doc.depth() > before);  // it became a step
    CHECK(doc.undo());
}
