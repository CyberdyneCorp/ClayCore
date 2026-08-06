#include <doctest/doctest.h>

#include <cstring>

#include "clay.h"

// openspec add-voxel-undo: voxel edits on document layers journal cell
// diffs and interleave with scene-command steps in one undo history.
// Standalone grids stay outside history.

namespace {

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
    clay_voxel_grid* grid = nullptr;
    int32_t white = 0;

    Doc() {
        doc = clay_document_create();
        REQUIRE(doc != nullptr);
        REQUIRE(clay_document_add_voxel_layer(doc, "Voxels", 0.1f, &layer, &grid) == CLAY_OK);
        const float color[3] = {1, 1, 1};
        REQUIRE(clay_voxel_palette_add(grid, color, &white) == CLAY_OK);
        REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(doc); }
};

clay_brush_params brush(int32_t size) {
    clay_brush_params b;
    std::memset(&b, 0, sizeof b);
    b.struct_size = static_cast<uint32_t>(sizeof b);
    b.size = size;
    b.shape = CLAY_BRUSH_SHAPE_CUBE;
    b.falloff = CLAY_BRUSH_FALLOFF_CONSTANT;
    b.strength = 1.0f;
    return b;
}

int32_t cell_at(clay_voxel_grid* grid, int32_t x, int32_t y, int32_t z) {
    const int32_t cell[3] = {x, y, z};
    int32_t index = -1;
    REQUIRE(clay_voxel_get(grid, cell, &index) == CLAY_OK);
    return index;
}

size_t occupied(clay_voxel_grid* grid) {
    size_t n = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &n) == CLAY_OK);
    return n;
}

}  // namespace

TEST_CASE("undo removes a brush stamp and redo restores it exactly") {
    Doc d;
    clay_brush_params b = brush(3);
    const int32_t cell[3] = {4, 5, 6};
    REQUIRE(clay_voxel_set_brush(d.grid, cell, &b, d.white) == CLAY_OK);
    size_t stamped = occupied(d.grid);
    CHECK(stamped == 27);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(occupied(d.grid) == 0);
    CHECK(cell_at(d.grid, 4, 5, 6) == 0);

    int32_t redone = 0;
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(redone == 1);
    CHECK(occupied(d.grid) == stamped);
    CHECK(cell_at(d.grid, 4, 5, 6) == d.white);
}

TEST_CASE("undo ordering interleaves voxel and scene edits") {
    Doc d;
    // A scene step (layer add), then a voxel step.
    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Clay", &sdf) == CLAY_OK);
    clay_brush_params b = brush(1);
    const int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set_brush(d.grid, cell, &b, d.white) == CLAY_OK);

    // First undo pops the voxel stamp, not the scene edit.
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(occupied(d.grid) == 0);
    size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 1);  // the layer add remains

    // Second undo pops the scene edit.
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 0);

    // Redo replays in order: scene first, voxel second.
    int32_t redone = 0;
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(occupied(d.grid) == 0);
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(occupied(d.grid) == 1);
}

TEST_CASE("grouped brush calls undo as one step with first-touch inverses") {
    Doc d;
    clay_brush_params b = brush(1);
    REQUIRE(clay_document_begin_undo_group(d.doc) == CLAY_OK);
    for (int32_t x = 0; x < 5; ++x) {
        const int32_t cell[3] = {x, 0, 0};
        REQUIRE(clay_voxel_set_brush(d.grid, cell, &b, d.white) == CLAY_OK);
    }
    // Same cell touched again inside the group: erase it. The merged diff
    // must remember the ORIGINAL before-value (empty), not the interim one.
    const int32_t first[3] = {0, 0, 0};
    REQUIRE(clay_voxel_erase_brush(d.grid, first, &b) == CLAY_OK);
    REQUIRE(clay_document_end_undo_group(d.doc) == CLAY_OK);
    CHECK(occupied(d.grid) == 4);

    size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 1);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(occupied(d.grid) == 0);

    int32_t redone = 0;
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(occupied(d.grid) == 4);
    CHECK(cell_at(d.grid, 0, 0, 0) == 0);
    CHECK(cell_at(d.grid, 1, 0, 0) == d.white);
}

TEST_CASE("standalone grids stay outside history") {
    Doc d;
    clay_voxel_grid* standalone = clay_voxel_grid_create(0.1f);
    REQUIRE(standalone != nullptr);
    const float color[3] = {1, 0, 0};
    int32_t red = 0;
    REQUIRE(clay_voxel_palette_add(standalone, color, &red) == CLAY_OK);
    clay_brush_params b = brush(1);
    const int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set_brush(standalone, cell, &b, red) == CLAY_OK);

    size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 0);
    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 0);
    CHECK(occupied(standalone) == 1);
    REQUIRE(clay_voxel_grid_destroy(standalone) == CLAY_OK);
}

TEST_CASE("depth counts voxel steps and fresh edits clear voxel redo") {
    Doc d;
    clay_brush_params b = brush(1);
    const int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set_brush(d.grid, cell, &b, d.white) == CLAY_OK);
    size_t undo_depth = 0, redo_depth = 0;
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(undo_depth == 1);
    CHECK(redo_depth == 0);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(undo_depth == 0);
    CHECK(redo_depth == 1);

    // A fresh SCENE edit invalidates the voxel redo (and vice versa).
    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Clay", &sdf) == CLAY_OK);
    int32_t redone = 0;
    REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
    CHECK(redone == 0);
    CHECK(occupied(d.grid) == 0);
}

TEST_CASE("fills, mirrored stamps and single cells journal their diffs") {
    Doc d;
    const int32_t a[3] = {0, 0, 0}, b3[3] = {2, 1, 0};
    REQUIRE(clay_voxel_fill_box(d.grid, a, b3, d.white) == CLAY_OK);
    CHECK(occupied(d.grid) == 6);
    const int32_t m[3] = {3, 0, 0};
    REQUIRE(clay_voxel_set_mirrored(d.grid, m, d.white, CLAY_MIRROR_X) == CLAY_OK);
    CHECK(occupied(d.grid) == 8);
    const int32_t single[3] = {9, 9, 9};
    REQUIRE(clay_voxel_set(d.grid, single, d.white) == CLAY_OK);
    CHECK(occupied(d.grid) == 9);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);  // single
    CHECK(occupied(d.grid) == 8);
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);  // mirrored pair
    CHECK(occupied(d.grid) == 6);
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);  // fill
    CHECK(occupied(d.grid) == 0);
}

TEST_CASE("a no-op brush records no undo step") {
    Doc d;
    clay_brush_params b = brush(3);
    const int32_t cell[3] = {0, 0, 0};
    // Erasing empty space changes nothing — no step must appear.
    REQUIRE(clay_voxel_erase_brush(d.grid, cell, &b) == CLAY_OK);
    size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(d.doc, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 0);
}

TEST_CASE("sculpt verbs journal too") {
    Doc d;
    clay_brush_params b = brush(5);
    const int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set_brush(d.grid, cell, &b, d.white) == CLAY_OK);
    size_t before = occupied(d.grid);

    // Inflate grows the blob; undo returns it exactly.
    clay_brush_params wide = brush(9);
    REQUIRE(clay_voxel_sculpt_inflate(d.grid, cell, &wide, 1) == CLAY_OK);
    size_t inflated = occupied(d.grid);
    CHECK(inflated > before);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(occupied(d.grid) == before);
}
