#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

// The C ABI mask surface (c-abi spec: the mask field). Same standard as the
// voxel cases: each edit runs twice, once through the C boundary and once on
// the engine types the way the Python bindings do, and the two have to agree
// cell for cell. A mask that gated differently depending on which binding drove
// it would be worse than no mask at all.

using namespace clay;

namespace {

struct CMask {
    clay_mask* mask = nullptr;
    explicit CMask(float cell_size = 0.1f) : mask(clay_mask_create(cell_size)) {
        REQUIRE(mask != nullptr);
    }
    ~CMask() { clay_mask_destroy(mask); }
    CMask(const CMask&) = delete;
    CMask& operator=(const CMask&) = delete;
};

struct CGridOwned {
    clay_voxel_grid* grid = nullptr;
    explicit CGridOwned(float voxel_size = 0.1f) : grid(clay_voxel_grid_create(voxel_size)) {
        REQUIRE(grid != nullptr);
    }
    ~CGridOwned() { clay_voxel_grid_destroy(grid); }
    CGridOwned(const CGridOwned&) = delete;
    CGridOwned& operator=(const CGridOwned&) = delete;
};

clay_brush_params brush(std::int32_t size, std::int32_t shape, std::int32_t falloff,
                        float strength, std::uint32_t seed, const clay_mask* mask = nullptr) {
    clay_brush_params b;
    std::memset(&b, 0, sizeof b);
    b.struct_size = static_cast<std::uint32_t>(sizeof b);
    b.size = size;
    b.shape = shape;
    b.falloff = falloff;
    b.strength = strength;
    b.seed = seed;
    b.mask = mask;
    return b;
}

voxel::BrushParams engine_brush(const clay_brush_params& b, const voxel::MaskField* mask) {
    voxel::BrushParams p;
    p.size = b.size;
    p.shape = static_cast<voxel::BrushShape>(b.shape);
    p.falloff = static_cast<voxel::BrushFalloff>(b.falloff);
    p.strength = b.strength;
    p.seed = b.seed;
    p.mask = mask;
    return p;
}

const float kWhite[3] = {1.0f, 1.0f, 1.0f};

void fill_block(clay_voxel_grid* g, std::int32_t lo, std::int32_t hi, std::int32_t index) {
    std::int32_t a[3] = {lo, lo, lo}, b[3] = {hi, hi, hi};
    REQUIRE(clay_voxel_fill_box(g, a, b, index) == CLAY_OK);
}

}  // namespace

TEST_CASE("c mask: painting matches the engine") {
    CMask c;
    voxel::MaskField engine(0.1f);

    clay_brush_params b = brush(9, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, 1.0f, 0);
    float point[3] = {0.35f, -0.15f, 0.05f};
    REQUIRE(clay_mask_paint(c.mask, point, &b, 1.0f) == CLAY_OK);
    engine.paint(kernel::cf3(point[0], point[1], point[2]), engine_brush(b, nullptr), 1.0f);

    std::size_t count = 0;
    REQUIRE(clay_mask_painted_count(c.mask, &count) == CLAY_OK);
    CHECK(count == engine.painted_count());
    CHECK(count > 0);

    for (int x = -2; x <= 8; ++x) {
        std::int32_t cell[3] = {x, -1, 0};
        float v = -1.0f;
        REQUIRE(clay_mask_get(c.mask, cell, &v) == CLAY_OK);
        CHECK(v == doctest::Approx(engine.get({x, -1, 0})));
    }

    std::int32_t empty = 1;
    REQUIRE(clay_mask_empty(c.mask, &empty) == CLAY_OK);
    CHECK(empty == 0);
}

TEST_CASE("c mask: batch sampling matches one-at-a-time") {
    CMask c;
    clay_brush_params b = brush(7, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_LINEAR, 1.0f, 0);
    std::int32_t centre[3] = {0, 0, 0};
    REQUIRE(clay_mask_paint_cell(c.mask, centre, &b, 1.0f) == CLAY_OK);

    std::vector<float> pts;
    for (int i = -6; i <= 6; ++i) {
        pts.push_back(i * 0.05f);
        pts.push_back(0.05f);
        pts.push_back(0.05f);
    }
    std::vector<float> batch(pts.size() / 3, -1.0f);
    REQUIRE(clay_mask_sample_many(c.mask, pts.data(), batch.size(), batch.data()) == CLAY_OK);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        float one = -1.0f;
        REQUIRE(clay_mask_sample(c.mask, &pts[i * 3], &one) == CLAY_OK);
        CHECK(batch[i] == doctest::Approx(one));
    }
}

TEST_CASE("c mask: a masked brush freezes, and matches the engine") {
    CMask c;
    voxel::MaskField engine(0.1f);
    for (int x = -8; x < 0; ++x)
        for (int y = -3; y <= 3; ++y)
            for (int z = -3; z <= 3; ++z) {
                std::int32_t cell[3] = {x, y, z};
                REQUIRE(clay_mask_set(c.mask, cell, 1.0f) == CLAY_OK);
                engine.set({x, y, z}, 1.0f);
            }

    CGridOwned cg;
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(cg.grid, kWhite, &index) == CLAY_OK);
    fill_block(cg.grid, -8, 8, index);

    voxel::VoxelGrid eg(0.1f);
    std::uint8_t eidx = eg.palette_add(kernel::cf3(1, 1, 1));
    eg.fill_box({-8, -8, -8}, {8, 8, 8}, eidx);

    clay_brush_params b =
        brush(20, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, 1.0f, 11, c.mask);
    std::int32_t centre[3] = {0, 0, 0};
    REQUIRE(clay_voxel_erase_brush(cg.grid, centre, &b) == CLAY_OK);
    eg.erase_brush({0, 0, 0}, engine_brush(b, &engine));

    std::size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(cg.grid, &occupied) == CLAY_OK);
    CHECK(occupied == eg.occupied_count());

    // The masked half is intact through the C boundary too.
    for (int x = -8; x < 0; ++x) {
        std::int32_t cell[3] = {x, 0, 0};
        std::int32_t v = 0;
        REQUIRE(clay_voxel_get(cg.grid, cell, &v) == CLAY_OK);
        CHECK(v != 0);
    }
}

TEST_CASE("c mask: a brush with a null mask is the pre-0.12.0 brush") {
    CGridOwned a, b;
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(a.grid, kWhite, &index) == CLAY_OK);
    REQUIRE(clay_voxel_palette_add(b.grid, kWhite, &index) == CLAY_OK);
    fill_block(a.grid, -5, 5, index);
    fill_block(b.grid, -5, 5, index);

    clay_brush_params full =
        brush(7, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, 0.7f, 5, nullptr);
    // A caller compiled against 0.11.0 declares a struct one field shorter;
    // the prefix rule zero-fills the mask, so the stamp is unchanged.
    clay_brush_params older = full;
    older.struct_size = static_cast<std::uint32_t>(offsetof(clay_brush_params, mask));

    std::int32_t centre[3] = {0, 0, 0};
    REQUIRE(clay_voxel_erase_brush(a.grid, centre, &full) == CLAY_OK);
    REQUIRE(clay_voxel_erase_brush(b.grid, centre, &older) == CLAY_OK);

    std::size_t ca = 0, cb = 0;
    REQUIRE(clay_voxel_occupied_count(a.grid, &ca) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(b.grid, &cb) == CLAY_OK);
    CHECK(ca == cb);
    for (int x = -5; x <= 5; ++x) {
        std::int32_t cell[3] = {x, 0, 0};
        std::int32_t va = 0, vb = 0;
        REQUIRE(clay_voxel_get(a.grid, cell, &va) == CLAY_OK);
        REQUIRE(clay_voxel_get(b.grid, cell, &vb) == CLAY_OK);
        CHECK(va == vb);
    }
}

TEST_CASE("c mask: region operations") {
    CMask c;
    voxel::MaskField engine(0.1f);
    clay_brush_params b = brush(9, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_LINEAR, 1.0f, 0);
    std::int32_t centre[3] = {0, 0, 0};
    REQUIRE(clay_mask_paint_cell(c.mask, centre, &b, 1.0f) == CLAY_OK);
    engine.paint(voxel::VoxelCoord{0, 0, 0}, engine_brush(b, nullptr), 1.0f);

    auto agree = [&]() {
        std::size_t count = 0;
        REQUIRE(clay_mask_painted_count(c.mask, &count) == CLAY_OK);
        CHECK(count == engine.painted_count());
    };

    REQUIRE(clay_mask_expand(c.mask, 1) == CLAY_OK);
    engine.expand(1);
    agree();
    REQUIRE(clay_mask_contract(c.mask, 1) == CLAY_OK);
    engine.contract(1);
    agree();
    REQUIRE(clay_mask_smooth(c.mask, 2) == CLAY_OK);
    engine.smooth(2);
    agree();
    REQUIRE(clay_mask_invert(c.mask) == CLAY_OK);
    engine.invert();
    agree();

    std::int32_t lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, has = 0;
    REQUIRE(clay_mask_bounds(c.mask, lo, hi, &has) == CLAY_OK);
    CHECK(has == 1);

    REQUIRE(clay_mask_clear(c.mask) == CLAY_OK);
    std::int32_t empty = 0;
    REQUIRE(clay_mask_empty(c.mask, &empty) == CLAY_OK);
    CHECK(empty == 1);
    REQUIRE(clay_mask_bounds(c.mask, lo, hi, &has) == CLAY_OK);
    CHECK(has == 0);
}

TEST_CASE("c mask: document-owned masks") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);

    clay_mask* missing = nullptr;
    CHECK(clay_document_mask(doc, layer, &missing) == CLAY_ERROR_NOT_FOUND);

    clay_mask* m = nullptr;
    REQUIRE(clay_document_add_mask(doc, layer, 0.1f, &m) == CLAY_OK);
    REQUIRE(m != nullptr);

    // The document owns it: destroying it through the caller-owned path is
    // rejected rather than obeyed, exactly as for a borrowed grid.
    CHECK(clay_mask_destroy(m) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_brush_params b = brush(5, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    std::int32_t centre[3] = {0, 0, 0};
    REQUIRE(clay_mask_paint_cell(m, centre, &b, 1.0f) == CLAY_OK);

    // A second lookup is the same handle onto the same field.
    clay_mask* again = nullptr;
    REQUIRE(clay_document_mask(doc, layer, &again) == CLAY_OK);
    CHECK(again == m);
    std::size_t count = 0;
    REQUIRE(clay_mask_painted_count(again, &count) == CLAY_OK);
    CHECK(count > 0);

    CHECK(clay_document_add_mask(doc, 999, 0.1f, nullptr) == CLAY_ERROR_NOT_FOUND);

    // After removal the borrowed handle fails rather than dangling.
    REQUIRE(clay_document_remove_mask(doc, layer) == CLAY_OK);
    CHECK(clay_document_remove_mask(doc, layer) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_mask_painted_count(m, &count) == CLAY_ERROR_NOT_FOUND);

    clay_document_destroy(doc);
}

TEST_CASE("c mask: invalid arguments are rejected") {
    CHECK(clay_mask_create(0.0f) == nullptr);
    CHECK(clay_mask_create(-1.0f) == nullptr);
    CHECK(clay_mask_destroy(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    CMask c;
    std::size_t count = 0;
    CHECK(clay_mask_painted_count(nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_get(c.mask, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_sample(c.mask, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_expand(c.mask, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_contract(c.mask, -1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_smooth(c.mask, 0) == CLAY_ERROR_INVALID_ARGUMENT);

    std::int32_t cell[3] = {0, 0, 0};
    CHECK(clay_mask_paint_cell(c.mask, cell, nullptr, 1.0f) == CLAY_ERROR_INVALID_ARGUMENT);

    // A brush naming a null-handle mask is an invalid brush, not a silent
    // ungated stamp.
    clay_brush_params b = brush(3, CLAY_BRUSH_SHAPE_CUBE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    b.struct_size = 4;  // below the original layout
    CGridOwned g;
    CHECK(clay_voxel_erase_brush(g.grid, cell, &b) == CLAY_ERROR_INVALID_ARGUMENT);
}
