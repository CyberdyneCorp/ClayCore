#include <doctest/doctest.h>

#include <vector>

#include "clay.h"
#include "clay/voxel/grid.h"

// Resolution levels across the C ABI (c-abi spec). The addition is purely
// additive, so the case that matters most is the one that mentions no level at
// all: it has to behave exactly as it did before these calls existed.

using namespace clay;

namespace {

struct CGrid {
    clay_voxel_grid* grid = nullptr;
    explicit CGrid(float voxel_size = 0.1f) : grid(clay_voxel_grid_create(voxel_size)) {
        REQUIRE(grid != nullptr);
    }
    ~CGrid() { clay_voxel_grid_destroy(grid); }
    operator clay_voxel_grid*() const { return grid; }
};

}  // namespace

TEST_CASE("c abi: a caller that never mentions a level gets today's grid") {
    CGrid g(0.1f);
    int32_t red = 0;
    const float rgb[3] = {1, 0, 0};
    REQUIRE(clay_voxel_palette_add(g, rgb, &red) == CLAY_OK);
    const int32_t a[3] = {0, 0, 0}, b[3] = {3, 3, 3};
    REQUIRE(clay_voxel_fill_box(g, a, b, red) == CLAY_OK);

    size_t levels = 0, active = 99;
    CHECK(clay_voxel_level_count(g, &levels) == CLAY_OK);
    CHECK(levels == 1);
    CHECK(clay_voxel_active_level(g, &active) == CLAY_OK);
    CHECK(active == 0);

    float size = 0.0f;
    CHECK(clay_voxel_size(g, &size) == CLAY_OK);
    CHECK(size == doctest::Approx(0.1f));
    size_t occupied = 0;
    CHECK(clay_voxel_occupied_count(g, &occupied) == CLAY_OK);
    CHECK(occupied == 64);

    // The refusals, which are what keeps a mistake from reading as an answer.
    CHECK(clay_voxel_drop_level(g) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_set_active_level(g, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_level_voxel_size(g, 1, &size) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_level_occupied_count(g, 1, &occupied) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_level_count(g, &levels) == CLAY_OK);
    CHECK(levels == 1);  // and none of them changed the grid
}

TEST_CASE("c abi: levels add, select and drop, and match the engine") {
    CGrid g(0.2f);
    int32_t body = 0;
    const float rgb[3] = {0.5f, 0.5f, 0.5f};
    REQUIRE(clay_voxel_palette_add(g, rgb, &body) == CLAY_OK);
    const int32_t a[3] = {0, 0, 0}, b[3] = {3, 3, 3};
    REQUIRE(clay_voxel_fill_box(g, a, b, body) == CLAY_OK);

    size_t fine = 0;
    REQUIRE(clay_voxel_add_level(g, &fine) == CLAY_OK);
    CHECK(fine == 1);

    float coarse_size = 0.0f, fine_size = 0.0f;
    CHECK(clay_voxel_level_voxel_size(g, 0, &coarse_size) == CLAY_OK);
    CHECK(clay_voxel_level_voxel_size(g, 1, &fine_size) == CLAY_OK);
    CHECK(coarse_size == doctest::Approx(2.0f * fine_size));

    size_t coarse_cells = 0, fine_cells = 0;
    CHECK(clay_voxel_level_occupied_count(g, 0, &coarse_cells) == CLAY_OK);
    CHECK(clay_voxel_level_occupied_count(g, 1, &fine_cells) == CLAY_OK);
    CHECK(fine_cells == coarse_cells * 8);  // subdivided, not resampled

    // Detail at the fine level, then a broad stroke at the coarse one: the
    // engine-side test states the property, this states that the ABI reaches it.
    REQUIRE(clay_voxel_set_active_level(g, 1) == CLAY_OK);
    const int32_t notch[3] = {2, 2, 2};
    REQUIRE(clay_voxel_erase(g, notch) == CLAY_OK);
    REQUIRE(clay_voxel_set_active_level(g, 0) == CLAY_OK);
    const int32_t lid_a[3] = {0, 4, 0}, lid_b[3] = {3, 4, 3};
    REQUIRE(clay_voxel_fill_box(g, lid_a, lid_b, body) == CLAY_OK);

    REQUIRE(clay_voxel_set_active_level(g, 1) == CLAY_OK);
    int32_t value = -1;
    CHECK(clay_voxel_get(g, notch, &value) == CLAY_OK);
    CHECK(value == 0);  // the detail is still there
    const int32_t lid_cell[3] = {0, 9, 0};
    CHECK(clay_voxel_get(g, lid_cell, &value) == CLAY_OK);
    CHECK(value == body);  // and the broad stroke arrived

    // Dropping takes the finest level and lands the active one on what is left.
    CHECK(clay_voxel_drop_level(g) == CLAY_OK);
    size_t levels = 0, active = 99;
    CHECK(clay_voxel_level_count(g, &levels) == CLAY_OK);
    CHECK(clay_voxel_active_level(g, &active) == CLAY_OK);
    CHECK(levels == 1);
    CHECK(active == 0);
}

TEST_CASE("c abi: the level stack cap is a refusal, not a silent no-op") {
    // Empty, so reaching the cap costs nothing: every level would otherwise
    // hold eight times the cells of the one below.
    CGrid g(1.0f);
    for (size_t i = 1; i < voxel::VoxelGrid::kMaxLevels; ++i) {
        size_t level = 0;
        REQUIRE(clay_voxel_add_level(g, &level) == CLAY_OK);
        REQUIRE(level == i);
    }
    size_t level = 12345;
    CHECK(clay_voxel_add_level(g, &level) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(level == 12345);  // the out parameter is left alone on refusal

    size_t levels = 0;
    CHECK(clay_voxel_level_count(g, &levels) == CLAY_OK);
    CHECK(levels == voxel::VoxelGrid::kMaxLevels);
}

TEST_CASE("c abi: a level refined over a region reads its parent outside it") {
    // The additive surface for regional refinement. What has to hold across
    // the boundary is the same thing that holds inside the engine: an
    // unrefined chunk reads its parent, so the solid does not change.
    CGrid g(0.1f);
    int32_t body = 0;
    const float rgb[3] = {0.5f, 0.5f, 0.5f};
    REQUIRE(clay_voxel_palette_add(g, rgb, &body) == CLAY_OK);
    // A plate wide enough to span many chunks at the fine level, which is
    // where a region is worth having.
    const int32_t a[3] = {-100, 0, -100}, b[3] = {100, 1, 100};
    REQUIRE(clay_voxel_fill_box(g, a, b, body) == CLAY_OK);

    size_t coarse_cells = 0;
    REQUIRE(clay_voxel_level_occupied_count(g, 0, &coarse_cells) == CLAY_OK);
    REQUIRE(coarse_cells > 0);

    const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.4f, 0.1f, 0.4f};
    size_t fine = 0;
    REQUIRE(clay_voxel_add_level_region(g, lo, hi, &fine) == CLAY_OK);
    CHECK(fine == 1);

    int32_t whole = -1;
    CHECK(clay_voxel_level_is_whole(g, 0, &whole) == CLAY_OK);
    CHECK(whole == 1);
    CHECK(clay_voxel_level_is_whole(g, 1, &whole) == CLAY_OK);
    CHECK(whole == 0);

    // The level HAS all the material — occupied counts describe the solid, so
    // this is the same 8x a whole level reports. What it does not have is the
    // storage: that is the chunk count, and it is a fraction of what a whole
    // level would need.
    size_t fine_cells = 0, chunks = 0, whole_chunks = 0;
    CHECK(clay_voxel_level_occupied_count(g, 1, &fine_cells) == CLAY_OK);
    CHECK(clay_voxel_level_chunk_count(g, 1, &chunks) == CLAY_OK);
    CHECK(clay_voxel_level_chunk_count(g, 0, &whole_chunks) == CLAY_OK);
    CHECK(fine_cells == coarse_cells * 8);
    CHECK(chunks > 0);
    // A whole fine level needs eight chunks per coarse one; this needs one.
    CHECK(chunks * 10 < whole_chunks * 8);

    // ...and a fine cell far outside the region still reads as material,
    // because it reads its parent.
    REQUIRE(clay_voxel_set_active_level(g, 1) == CLAY_OK);
    const int32_t outside[3] = {-150, 1, -150};
    int32_t value = 0;
    CHECK(clay_voxel_get(g, outside, &value) == CLAY_OK);
    CHECK(value == body);
}

TEST_CASE("c abi: an empty or null region is refused and the grid is untouched") {
    CGrid g(0.1f);
    const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.4f, 0.4f, 0.4f};
    size_t level = 99;
    CHECK(clay_voxel_add_level_region(g, nullptr, hi, &level) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_add_level_region(g, lo, nullptr, &level) == CLAY_ERROR_INVALID_ARGUMENT);
    // An inverted box is not a region.
    CHECK(clay_voxel_add_level_region(g, hi, lo, &level) == CLAY_ERROR_INVALID_ARGUMENT);
    size_t count = 0;
    CHECK(clay_voxel_level_count(g, &count) == CLAY_OK);
    CHECK(count == 1);

    // And the reporting calls refuse a level that is not there.
    size_t chunks = 0;
    int32_t whole = 0;
    CHECK(clay_voxel_level_chunk_count(g, 5, &chunks) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_level_is_whole(g, 5, &whole) == CLAY_ERROR_INVALID_ARGUMENT);
}
