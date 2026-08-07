#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/brush/mask_extrude.h"
#include "clay/brush/stroke.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

// The mask brush and mask extrude across the C ABI (c-abi spec,
// add-mask-stroke-brush and add-mask-extrude). Same standard as the rest of the
// mask surface: an operation runs through the boundary and again on the engine
// types the way the Python bindings do, and the two have to agree.

using namespace clay;

namespace {

struct CMask {
    clay_mask* mask = nullptr;
    explicit CMask(float cell_size = 0.05f) : mask(clay_mask_create(cell_size)) {
        REQUIRE(mask != nullptr);
    }
    ~CMask() { clay_mask_destroy(mask); }
    CMask(const CMask&) = delete;
    CMask& operator=(const CMask&) = delete;
};

struct CGrid {
    clay_voxel_grid* grid = nullptr;
    explicit CGrid(float voxel_size = 0.05f) : grid(clay_voxel_grid_create(voxel_size)) {
        REQUIRE(grid != nullptr);
    }
    ~CGrid() { clay_voxel_grid_destroy(grid); }
    CGrid(const CGrid&) = delete;
    CGrid& operator=(const CGrid&) = delete;
};

clay_stroke_preset preset(float radius, float spacing) {
    clay_stroke_preset p;
    REQUIRE(clay_stroke_preset_defaults(&p) == CLAY_OK);
    p.radius = radius;
    p.spacing = spacing;
    return p;
}

// A drag along X as the ABI wants it: five floats per sample.
std::vector<float> drag_x(float from, float to, int count = 16) {
    std::vector<float> out;
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        out.push_back(from + (to - from) * t);
        out.push_back(0.0f);
        out.push_back(0.0f);
        out.push_back(1.0f);  // pressure
        out.push_back(0.0f);  // tilt
    }
    return out;
}

clay_mask_extrude_params extrude(float thickness, std::int32_t side = CLAY_EXTRUDE_OUTWARD) {
    clay_mask_extrude_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.thickness = thickness;
    p.side = side;
    return p;
}

// A ball of radius r into a grid, and the same cap masked on both sides of the
// boundary, so the C and engine paths start from identical input.
void fill_ball(clay_voxel_grid* grid, float voxel_size, float r) {
    std::int32_t index = 0;
    const float colour[3] = {0.8f, 0.2f, 0.2f};
    REQUIRE(clay_voxel_palette_add(grid, colour, &index) == CLAY_OK);
    const auto n = static_cast<std::int32_t>(std::ceil(r / voxel_size)) + 2;
    for (std::int32_t z = -n; z <= n; ++z)
        for (std::int32_t y = -n; y <= n; ++y)
            for (std::int32_t x = -n; x <= n; ++x) {
                const float cx = (static_cast<float>(x) + 0.5f) * voxel_size;
                const float cy = (static_cast<float>(y) + 0.5f) * voxel_size;
                const float cz = (static_cast<float>(z) + 0.5f) * voxel_size;
                if (std::sqrt(cx * cx + cy * cy + cz * cz) > r) continue;
                const std::int32_t cell[3] = {x, y, z};
                REQUIRE(clay_voxel_set(grid, cell, index) == CLAY_OK);
            }
}

}  // namespace

TEST_CASE("c mask brush: a stroke paints the same mask through both bindings") {
    const std::vector<float> samples = drag_x(-0.4f, 0.4f);
    const clay_stroke_preset p = preset(0.15f, 0.3f);

    CMask c(0.05f);
    std::size_t applied = 0;
    REQUIRE(clay_mask_apply_stroke(c.mask, samples.data(), samples.size() / 5, &p, 1.0f,
                                   CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH,
                                   &applied) == CLAY_OK);
    CHECK(applied > 1);

    voxel::MaskField engine(0.05f);
    std::vector<brush::StrokeSample> in;
    for (std::size_t i = 0; i < samples.size(); i += 5)
        in.push_back({kernel::cf3(samples[i], samples[i + 1], samples[i + 2]), samples[i + 3],
                      samples[i + 4]});
    brush::StrokePreset ep;
    ep.radius = 0.15f;
    ep.spacing = 0.3f;
    CHECK(brush::apply_to_mask(engine, brush::resolve_stroke(in, ep), 1.0f) == applied);

    std::size_t painted = 0;
    REQUIRE(clay_mask_painted_count(c.mask, &painted) == CLAY_OK);
    CHECK(painted == engine.painted_count());

    for (float x = -0.35f; x <= 0.35f; x += 0.1f) {
        const float point[3] = {x, 0.0f, 0.0f};
        float value = 0.0f;
        REQUIRE(clay_mask_sample(c.mask, point, &value) == CLAY_OK);
        CHECK(value == doctest::Approx(engine.sample(kernel::cf3(x, 0, 0))));
        CHECK(value > 0.5f);
    }
}

TEST_CASE("c mask brush: fill and the bounded invert") {
    CMask c(0.1f);
    const float lo[3] = {-0.3f, -0.3f, -0.3f};
    const float hi[3] = {0.3f, 0.3f, 0.3f};

    REQUIRE(clay_mask_fill(c.mask, lo, hi, 1.0f) == CLAY_OK);
    float value = 0.0f;
    const float inside[3] = {0.05f, 0.05f, 0.05f};
    REQUIRE(clay_mask_sample(c.mask, inside, &value) == CLAY_OK);
    CHECK(value == doctest::Approx(1.0f));

    REQUIRE(clay_mask_invert_within(c.mask, lo, hi) == CLAY_OK);
    REQUIRE(clay_mask_sample(c.mask, inside, &value) == CLAY_OK);
    CHECK(value == doctest::Approx(0.0f));

    // Outside the box nothing happened, which is what separates this from
    // clay_mask_invert.
    const float outside[3] = {0.95f, 0.0f, 0.0f};
    REQUIRE(clay_mask_sample(c.mask, outside, &value) == CLAY_OK);
    CHECK(value == doctest::Approx(0.0f));
}

TEST_CASE("c mask brush: an unbounded or inverted box is refused") {
    CMask c;
    const float big = 3.4e38f;
    const float lo[3] = {-big, -big, -big};
    const float hi[3] = {big, big, big};
    CHECK(clay_mask_fill(c.mask, lo, hi, 1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_invert_within(c.mask, lo, hi) == CLAY_ERROR_INVALID_ARGUMENT);

    const float a[3] = {1.0f, 1.0f, 1.0f};
    const float b[3] = {-1.0f, -1.0f, -1.0f};
    CHECK(clay_mask_fill(c.mask, a, b, 1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_fill(c.mask, nullptr, b, 1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mask_apply_stroke(nullptr, nullptr, 0, nullptr, 1.0f, CLAY_BRUSH_SHAPE_SPHERE,
                                 CLAY_BRUSH_FALLOFF_SMOOTH, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c relax and flatten freeze against a mask, and an older caller does not") {
    // A volume to work on, imported the way an app would.
    const float positions[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0, 0, 0, 1};
    const std::uint32_t indices[] = {0, 1, 2, 0, 1, 3, 1, 2, 3, 2, 0, 3};
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &mesh) == CLAY_OK);
    clay_volume_params vp;
    std::memset(&vp, 0, sizeof vp);
    vp.struct_size = static_cast<std::uint32_t>(sizeof vp);
    vp.cell_size = 0.1f;

    CMask freeze(0.1f);
    const float lo[3] = {-2, -2, -2};
    const float hi[3] = {2, 2, 2};
    REQUIRE(clay_mask_fill(freeze.mask, lo, hi, 1.0f) == CLAY_OK);

    clay_relax_params rp;
    std::memset(&rp, 0, sizeof rp);
    rp.struct_size = static_cast<std::uint32_t>(sizeof rp);
    rp.strength = 1.0f;
    rp.radius_cells = 2;
    rp.iterations = 2;

    // Frozen everywhere: the samples come back exactly as they went in.
    clay_item* held = nullptr;
    REQUIRE(clay_item_volume_from_mesh(mesh, &vp, &held) == CLAY_OK);
    clay_item* moved = nullptr;
    REQUIRE(clay_item_volume_from_mesh(mesh, &vp, &moved) == CLAY_OK);

    rp.mask = freeze.mask;
    CHECK(clay_item_volume_relax(held, &rp) == CLAY_OK);
    rp.mask = nullptr;
    CHECK(clay_item_volume_relax(moved, &rp) == CLAY_OK);

    // A caller compiled before the mask field existed declares the older size;
    // the call must behave exactly as it always did rather than reading a
    // handle out of bytes it never wrote.
    clay_relax_params legacy = rp;
    legacy.struct_size =
        static_cast<std::uint32_t>(offsetof(clay_relax_params, falloff) + sizeof(float));
    clay_item* old_caller = nullptr;
    REQUIRE(clay_item_volume_from_mesh(mesh, &vp, &old_caller) == CLAY_OK);
    CHECK(clay_item_volume_relax(old_caller, &legacy) == CLAY_OK);

    clay_item_destroy(held);
    clay_item_destroy(moved);
    clay_item_destroy(old_caller);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c mask extrude: a plate comes off a layer, and off a grid") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);

    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_SPHERE;
    desc.params[0] = 0.6f;
    desc.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &desc, &node) == CLAY_OK);

    clay_mask* mask = nullptr;
    REQUIRE(clay_document_add_mask(doc, layer, 0.03f, &mask) == CLAY_OK);
    const float lo[3] = {-0.3f, 0.35f, -0.3f};
    const float hi[3] = {0.3f, 1.2f, 0.3f};
    REQUIRE(clay_mask_fill(mask, lo, hi, 1.0f) == CLAY_OK);

    const clay_mask_extrude_params ep = extrude(0.12f);
    clay_item* plate = nullptr;
    REQUIRE(clay_document_mask_extrude(doc, layer, mask, &ep, &plate) == CLAY_OK);
    REQUIRE(plate != nullptr);
    // It is an ordinary item: adding it to a layer is all a host has to do.
    clay_layer_id shell = 0;
    REQUIRE(clay_add_sdf_layer(doc, "plate", &shell) == CLAY_OK);
    clay_node_id placed = 0;
    CHECK(clay_layer_add_item(doc, shell, plate, &placed) == CLAY_OK);
    // add_item COPIES the composed edit and leaves the builder untouched, so
    // the item is still the caller's to destroy — the same rule every other
    // clay_item producer follows.
    clay_item_destroy(plate);

    // And the same verb on voxels, which owns its result.
    CGrid g(0.03f);
    fill_ball(g.grid, 0.03f, 0.6f);
    clay_voxel_grid* extract = nullptr;
    REQUIRE(clay_voxel_mask_extrude(g.grid, mask, &ep, &extract) == CLAY_OK);
    REQUIRE(extract != nullptr);
    std::size_t cells = 0;
    REQUIRE(clay_voxel_occupied_count(extract, &cells) == CLAY_OK);
    CHECK(cells > 0);
    CHECK(clay_voxel_grid_destroy(extract) == CLAY_OK);

    clay_document_destroy(doc);
}

TEST_CASE("c mask extrude: a refusal is typed and produces no handle") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_SPHERE;
    desc.params[0] = 0.6f;
    desc.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &desc, &node) == CLAY_OK);

    CMask empty(0.03f);
    const clay_mask_extrude_params ep = extrude(0.12f);
    clay_item* out = reinterpret_cast<clay_item*>(1);  // must be left alone
    CHECK(clay_document_mask_extrude(doc, layer, empty.mask, &ep, &out) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // Painted, but nowhere near the surface.
    CMask away(0.03f);
    const float lo[3] = {4.0f, 4.0f, 4.0f};
    const float hi[3] = {4.4f, 4.4f, 4.4f};
    REQUIRE(clay_mask_fill(away.mask, lo, hi, 1.0f) == CLAY_OK);
    CHECK(clay_document_mask_extrude(doc, layer, away.mask, &ep, &out) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A thickness that is not one, and a side that is not a side.
    clay_mask_extrude_params bad = extrude(0.0f);
    CHECK(clay_document_mask_extrude(doc, layer, away.mask, &bad, &out) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    bad = extrude(0.12f, 99);
    CHECK(clay_document_mask_extrude(doc, layer, away.mask, &bad, &out) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    bad = extrude(0.12f);
    bad.struct_size = 4;  // below the original layout
    CHECK(clay_document_mask_extrude(doc, layer, away.mask, &bad, &out) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_document_destroy(doc);
}

TEST_CASE("c mask extrude: the mask can be measured on its own") {
    CMask c(0.05f);
    const float lo[3] = {-0.3f, -0.3f, -0.3f};
    const float hi[3] = {0.3f, 0.3f, 0.3f};
    REQUIRE(clay_mask_fill(c.mask, lo, hi, 1.0f) == CLAY_OK);

    clay_item* measured = nullptr;
    REQUIRE(clay_mask_to_field(c.mask, 0.5f, 0.2f, 0.3f, 0.0f, &measured) == CLAY_OK);
    REQUIRE(measured != nullptr);
    clay_item_destroy(measured);

    CMask empty(0.05f);
    clay_item* nothing = nullptr;
    CHECK(clay_mask_to_field(empty.mask, 0.5f, 0.0f, 0.0f, 0.0f, &nothing) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}
