#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/voxel/grid.h"

// The C ABI surface for the remaining verbs and repair (c-abi spec). Same
// standard as the rest: each call runs twice, once through the boundary and
// once on the engine types the way the Python bindings do, and the grids have
// to agree cell for cell.

using namespace clay;
using kernel::cf3;

namespace {

clay_brush_params brush(std::int32_t size, std::int32_t shape = CLAY_BRUSH_SHAPE_CUBE) {
    clay_brush_params b;
    std::memset(&b, 0, sizeof b);
    b.struct_size = static_cast<std::uint32_t>(sizeof b);
    b.size = size;
    b.shape = shape;
    b.falloff = CLAY_BRUSH_FALLOFF_CONSTANT;
    b.strength = 1.0f;
    return b;
}

voxel::BrushParams engine_brush(const clay_brush_params& b) {
    voxel::BrushParams p;
    p.size = b.size;
    p.shape = static_cast<voxel::BrushShape>(b.shape);
    p.falloff = static_cast<voxel::BrushFalloff>(b.falloff);
    p.strength = b.strength;
    p.seed = b.seed;
    return p;
}

// A hollow shell through the C boundary, and its engine twin.
void fill_hollow(clay_voxel_grid* g, std::int32_t index, int half) {
    std::int32_t lo[3] = {-half, -half, -half}, hi[3] = {half, half, half};
    REQUIRE(clay_voxel_fill_box(g, lo, hi, index) == CLAY_OK);
    std::int32_t ilo[3] = {-half + 1, -half + 1, -half + 1};
    std::int32_t ihi[3] = {half - 1, half - 1, half - 1};
    REQUIRE(clay_voxel_fill_box(g, ilo, ihi, 0) == CLAY_OK);
}

voxel::VoxelGrid engine_hollow(int half) {
    voxel::VoxelGrid g(0.1f);
    std::uint8_t shell = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    g.fill_box({-half, -half, -half}, {half, half, half}, shell);
    g.fill_box({-half + 1, -half + 1, -half + 1}, {half - 1, half - 1, half - 1}, 0);
    return g;
}

}  // namespace

TEST_CASE("c verbs: each one matches the engine") {
    const float white[3] = {0.6f, 0.6f, 0.65f};
    auto run = [&](auto&& through_c, auto&& through_engine) {
        clay_voxel_grid* g = clay_voxel_grid_create(0.1f);
        REQUIRE(g != nullptr);
        std::int32_t index = 0;
        REQUIRE(clay_voxel_palette_add(g, white, &index) == CLAY_OK);
        std::int32_t lo[3] = {-8, 0, -8}, hi[3] = {8, 3, 8};
        REQUIRE(clay_voxel_fill_box(g, lo, hi, index) == CLAY_OK);

        voxel::VoxelGrid e(0.1f);
        std::uint8_t eindex = e.palette_add(cf3(white[0], white[1], white[2]));
        e.fill_box({-8, 0, -8}, {8, 3, 8}, eindex);

        through_c(g);
        through_engine(e);

        std::size_t count = 0;
        REQUIRE(clay_voxel_occupied_count(g, &count) == CLAY_OK);
        CHECK(count == e.occupied_count());
        for (int x = -8; x <= 8; x += 3)
            for (int y = 0; y <= 4; ++y) {
                std::int32_t cell[3] = {x, y, 0};
                std::int32_t v = 0;
                REQUIRE(clay_voxel_get(g, cell, &v) == CLAY_OK);
                CHECK((v != 0) == (e.get({x, y, 0}) != 0));
            }
        clay_voxel_grid_destroy(g);
    };

    SUBCASE("fill cavities") {
        clay_brush_params b = brush(9);
        std::int32_t at[3] = {0, 2, 0};
        run([&](clay_voxel_grid* g) {
                std::int32_t plo[3] = {0, 2, 0}, phi[3] = {0, 3, 0};
                REQUIRE(clay_voxel_fill_box(g, plo, phi, 0) == CLAY_OK);
                REQUIRE(clay_voxel_sculpt_fill_cavities(g, at, &b, 2) == CLAY_OK);
            },
            [&](voxel::VoxelGrid& e) {
                e.fill_box({0, 2, 0}, {0, 3, 0}, 0);
                e.sculpt_fill_cavities({0, 2, 0}, engine_brush(b), 2);
            });
    }

    SUBCASE("scrape") {
        clay_brush_params b = brush(13);
        std::int32_t at[3] = {0, 3, 0};
        const float up[3] = {0, 1, 0};
        run([&](clay_voxel_grid* g) {
                REQUIRE(clay_voxel_sculpt_scrape(g, at, &b, up, 0.0f) == CLAY_OK);
            },
            [&](voxel::VoxelGrid& e) {
                e.sculpt_scrape({0, 3, 0}, engine_brush(b), cf3(0, 1, 0), 0.0f);
            });
    }

    SUBCASE("smudge") {
        clay_brush_params b = brush(9);
        std::int32_t at[3] = {0, 3, 0};
        const float push[3] = {0.3f, 0, 0};
        run([&](clay_voxel_grid* g) {
                REQUIRE(clay_voxel_sculpt_smudge(g, at, &b, push) == CLAY_OK);
            },
            [&](voxel::VoxelGrid& e) {
                e.sculpt_smudge({0, 3, 0}, engine_brush(b), cf3(0.3f, 0, 0));
            });
    }

    SUBCASE("carve with an alpha") {
        clay_brush_params b = brush(9);
        std::int32_t at[3] = {0, 3, 0};
        std::vector<float> alpha(64, 0.0f);
        for (int j = 0; j < 8; ++j)
            for (int i = 4; i < 8; ++i) alpha[j * 8 + i] = 1.0f;
        const float down[3] = {0, 1, 0};
        run([&](clay_voxel_grid* g) {
                REQUIRE(clay_voxel_sculpt_carve_alpha(g, at, &b, alpha.data(), 8, 8, down, 0) ==
                        CLAY_OK);
            },
            [&](voxel::VoxelGrid& e) {
                e.sculpt_carve_alpha({0, 3, 0}, engine_brush(b), alpha.data(), 8, 8,
                                     cf3(0, 1, 0), 0);
            });
    }
}

TEST_CASE("c repair: report, close, fill") {
    clay_voxel_grid* g = clay_voxel_grid_create(0.1f);
    REQUIRE(g != nullptr);
    const float shell[3] = {0.8f, 0.4f, 0.2f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(g, shell, &index) == CLAY_OK);
    fill_hollow(g, index, 5);
    voxel::VoxelGrid e = engine_hollow(5);

    clay_repair_report report;
    std::memset(&report, 0, sizeof report);
    report.struct_size = static_cast<std::uint32_t>(sizeof report);
    REQUIRE(clay_voxel_repair_report(g, &report) == CLAY_OK);
    CHECK(report.enclosed_voids == 1);
    CHECK(report.void_cells == 9u * 9u * 9u);
    CHECK(report.largest_void == report.void_cells);
    CHECK(report.airtight == 0);
    CHECK(report.enclosed_voids == e.repair_report().enclosed_voids);

    SUBCASE("the report changes nothing") {
        std::size_t before = 0, after = 0;
        REQUIRE(clay_voxel_occupied_count(g, &before) == CLAY_OK);
        REQUIRE(clay_voxel_repair_report(g, &report) == CLAY_OK);
        REQUIRE(clay_voxel_occupied_count(g, &after) == CLAY_OK);
        CHECK(before == after);
    }

    SUBCASE("close then fill, matching the engine") {
        std::int32_t hole[3] = {5, 0, 0};
        REQUIRE(clay_voxel_set(g, hole, 0) == CLAY_OK);
        e.set({5, 0, 0}, 0);

        REQUIRE(clay_voxel_repair_close_holes(g, 1, nullptr) == CLAY_OK);
        e.repair_close_holes(1, nullptr);
        REQUIRE(clay_voxel_repair_fill_voids(g, nullptr) == CLAY_OK);
        e.repair_fill_voids(nullptr);

        std::size_t count = 0;
        REQUIRE(clay_voxel_occupied_count(g, &count) == CLAY_OK);
        CHECK(count == e.occupied_count());
        REQUIRE(clay_voxel_repair_report(g, &report) == CLAY_OK);
        CHECK(report.airtight == 1);

        std::int32_t centre[3] = {0, 0, 0};
        std::int32_t v = 0;
        REQUIRE(clay_voxel_get(g, centre, &v) == CLAY_OK);
        CHECK(v == index);  // coloured from the shell, not a new palette entry
    }

    clay_voxel_grid_destroy(g);
}

TEST_CASE("c repair: invalid arguments are refused") {
    clay_voxel_grid* g = clay_voxel_grid_create(0.1f);
    REQUIRE(g != nullptr);
    std::int32_t at[3] = {0, 0, 0};
    clay_brush_params b = brush(5);

    CHECK(clay_voxel_sculpt_fill_cavities(g, at, &b, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sculpt_fill_cavities(nullptr, at, &b, 1) == CLAY_ERROR_INVALID_ARGUMENT);

    const float zero[3] = {0, 0, 0};
    CHECK(clay_voxel_sculpt_scrape(g, at, &b, zero, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sculpt_scrape(g, at, &b, nullptr, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sculpt_smudge(g, at, &b, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    std::vector<float> alpha(16, 1.0f);
    const float up[3] = {0, 1, 0};
    CHECK(clay_voxel_sculpt_carve_alpha(g, at, &b, nullptr, 4, 4, up, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sculpt_carve_alpha(g, at, &b, alpha.data(), 0, 4, up, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sculpt_carve_alpha(g, at, &b, alpha.data(), 4, 4, zero, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sculpt_carve_alpha(g, at, &b, alpha.data(), 4, 4, up, 999) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    CHECK(clay_voxel_repair_close_holes(g, 0, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_repair_report(g, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_repair_report shorter;
    std::memset(&shorter, 0, sizeof shorter);
    shorter.struct_size = 4;  // below the original layout
    CHECK(clay_voxel_repair_report(g, &shorter) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_voxel_grid_destroy(g);
}
