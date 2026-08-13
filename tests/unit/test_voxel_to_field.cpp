// A voxel sculpt going back into the document as an operand (#90).
//
// The acceptance test the issue asks for is the last one here: block out with
// booleans, convert, sculpt with the voxel verbs, convert back, boolean again.
// The rest establish what the conversion preserves and what it does not,
// because the issue is explicit that this is a CONVERSION and not a view.
#include <doctest/doctest.h>

#include <cmath>
#include <optional>

#include "clay/field/volume.h"
#include "clay.h"
#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/voxel/grid.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

// A ball of voxels, which has a surface a distance can be checked against.
VoxelGrid ball(float cell, int radius, std::uint8_t* out_index = nullptr) {
    VoxelGrid g(cell);
    const std::uint8_t c = g.palette_add(kernel::cf3(0.8f, 0.5f, 0.3f));
    if (out_index) *out_index = c;
    g.set_brush({0, 0, 0}, radius * 2, c, voxel::BrushShape::Sphere);
    return g;
}

}  // namespace

TEST_CASE("a grid converts to a field without a mesh in between") {
    VoxelGrid g = ball(0.05f, 8);
    const std::optional<field::FieldVolume> v = g.to_field();
    REQUIRE(v.has_value());
    CHECK(v->sample_count() > 0);

    // Inside is negative, outside is positive, and the sign flips across the
    // surface — the property that makes this an operand rather than a picture.
    CHECK(v->eval(kernel::cf3(0, 0, 0)) < 0.0f);
    CHECK(v->eval(kernel::cf3(2.0f, 0, 0)) > 0.0f);
}

TEST_CASE("the field measures distance rather than occupancy") {
    // Without redistance the stored values are an occupancy ramp: it crosses
    // zero in the right place and says nothing truthful about how far away
    // anything is, so every blend downstream would work from a bad Lipschitz.
    const float cell = 0.05f;
    const int radius = 8;
    VoxelGrid g = ball(cell, radius);
    const std::optional<field::FieldVolume> v = g.to_field();
    REQUIRE(v.has_value());

    // The ball's surface is about radius*cell from the centre. A sample one
    // cell inside it should read about one cell of distance, not a fraction of
    // an occupancy step.
    const float r = static_cast<float>(radius) * cell;
    const float d_centre = v->eval(kernel::cf3(0, 0, 0));
    // Distance from the centre to the surface, as a distance: negative, and
    // within a couple of cells of the radius.
    CHECK(d_centre < 0.0f);
    CHECK(std::abs(std::abs(d_centre) - r) < 3.0f * cell);
}

TEST_CASE("the surface comes back within about a cell") {
    // The tolerance this conversion is held to, and it is stated as a DISTANCE
    // bound rather than a volume difference: a thin spike has near-zero volume
    // and large distance error, so a volume bound hides exactly the failure
    // that matters. One cell is the quantum the lattice imposes and nothing
    // here can beat it.
    const float cell = 0.05f;
    const int radius = 10;
    VoxelGrid g = ball(cell, radius);
    const std::optional<field::FieldVolume> v = g.to_field();
    REQUIRE(v.has_value());

    const float r = static_cast<float>(radius) * cell;
    float worst = 0.0f;
    // Sample the field where the voxel ball's surface is and see how far the
    // field thinks it is from a surface. Directions chosen off-axis, since an
    // axis-aligned probe is the easiest case for a lattice.
    const kernel::cfloat3 dirs[] = {
        kernel::cf3(1, 0, 0),          kernel::cf3(0, 1, 0),
        kernel::cf3(0, 0, 1),          kernel::cf3(0.577f, 0.577f, 0.577f),
        kernel::cf3(0.707f, 0.707f, 0), kernel::cf3(0, 0.707f, 0.707f),
    };
    for (const kernel::cfloat3& d : dirs) {
        const kernel::cfloat3 on_surface = d * r;
        worst = std::max(worst, std::abs(v->eval(on_surface)));
    }
    CHECK(worst < 2.0f * cell);
}

TEST_CASE("one palette index converts on its own, so colour can survive") {
    // A single field has nowhere to put a palette. Converting once per index
    // is what carries colour across: each part is placed with its own entry's
    // colour, and the union of the parts is the whole solid.
    VoxelGrid g(0.05f);
    const std::uint8_t red = g.palette_add(kernel::cf3(1, 0, 0));
    const std::uint8_t blue = g.palette_add(kernel::cf3(0, 0, 1));
    g.fill_box({-6, -6, -6}, {-1, 6, 6}, red);
    g.fill_box({0, -6, -6}, {6, 6, 6}, blue);

    const std::optional<field::FieldVolume> whole = g.to_field();
    const std::optional<field::FieldVolume> just_red =
        g.to_field(VoxelGrid::FieldOptions{0, 0.0f, red});
    const std::optional<field::FieldVolume> just_blue =
        g.to_field(VoxelGrid::FieldOptions{0, 0.0f, blue});
    REQUIRE(whole.has_value());
    REQUIRE(just_red.has_value());
    REQUIRE(just_blue.has_value());

    // Deep inside the red half: solid in the whole and in the red part, and
    // outside the blue part.
    const kernel::cfloat3 in_red = kernel::cf3(-0.2f, 0, 0);
    CHECK(whole->eval(in_red) < 0.0f);
    CHECK(just_red->eval(in_red) < 0.0f);
    CHECK(just_blue->eval(in_red) > 0.0f);

    const kernel::cfloat3 in_blue = kernel::cf3(0.2f, 0, 0);
    CHECK(whole->eval(in_blue) < 0.0f);
    CHECK(just_blue->eval(in_blue) < 0.0f);
    CHECK(just_red->eval(in_blue) > 0.0f);

    // A palette index nothing carries has nothing to convert.
    CHECK_FALSE(g.to_field(VoxelGrid::FieldOptions{0, 0.0f, 200}).has_value());
}

TEST_CASE("the conversion refuses what it cannot convert") {
    VoxelGrid empty(0.05f);
    CHECK_FALSE(empty.to_field().has_value());

    VoxelGrid g = ball(0.05f, 4);
    CHECK_FALSE(g.to_field(9).has_value());  // a level this grid does not have
}

TEST_CASE("converting does not touch the grid") {
    VoxelGrid g = ball(0.05f, 6);
    const std::vector<std::uint8_t> before = g.serialize();
    (void)g.to_field();
    (void)g.to_field(VoxelGrid::FieldOptions{2, 0.0f, 0});
    CHECK(g.serialize() == before);
}

TEST_CASE("a sculpt makes the return trip and can be booleaned again") {
    // The acceptance test #90 asks for: block out with booleans, convert to
    // voxels, sculpt with the voxel verbs, convert back, boolean again.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("blockout");
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), kernel::cf3(0, 0, 0)));
    l.sdf->insert(clay_test::item(scene::Prim::box(kernel::cf3(0.3f, 0.3f, 0.3f)),
                                  kernel::cf3(0.35f, 0, 0), scene::Op::Add,
                                  scene::Blend{scene::BlendProfile::Quadratic, 0.1f}));
    const scene::Tape tape = scene::compile_document(doc);

    // Into voxels, which is the direction that already worked.
    VoxelGrid g(0.02f);
    g.rasterize_tape(tape, math::Aabb{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)});
    REQUIRE(g.occupied_count() > 0);

    // Sculpt it with the verbs that only exist on this side.
    voxel::BrushParams brush;
    brush.size = 9;
    brush.shape = voxel::BrushShape::Sphere;
    g.sculpt_smooth({17, 0, 0}, brush);   // the seam the SDF side cannot smooth
    g.sculpt_inflate({0, 0, 0}, brush, 1);
    const std::size_t after_sculpt = g.occupied_count();
    REQUIRE(after_sculpt > 0);

    // And back out as an operand.
    const std::optional<field::FieldVolume> v = g.to_field();
    REQUIRE(v.has_value());

    // It is an operand, not a picture: place it and boolean against it.
    scene::Document round_trip;
    scene::Layer& out = round_trip.add_sdf_layer("converted");
    scene::Node placed;
    placed.prim = scene::Prim::volume();
    placed.volume = std::make_shared<const field::FieldVolume>(*v);
    out.sdf->insert(placed);
    scene::Node cut = clay_test::item(scene::Prim::sphere(0.25f), kernel::cf3(0, 0.45f, 0),
                                      scene::Op::Subtract);
    out.sdf->insert(cut);

    const scene::Tape back = scene::compile_document(round_trip);
    REQUIRE_FALSE(back.empty());

    // The boolean took: solid where the sculpt is, and carved where the
    // subtraction was placed.
    auto field_at = [&](kernel::cfloat3 p) {
        float d = 0;
        eval::PointQuery q{reinterpret_cast<const float*>(&p), 1, 1e-4f};
        eval::eval_points_reference(back, q, eval::PointResults{&d, nullptr, nullptr});
        return d;
    };
    CHECK(field_at(kernel::cf3(0, 0, 0)) < 0.0f);        // still solid at the core
    CHECK(field_at(kernel::cf3(0, 0.45f, 0)) > 0.0f);    // and cut where it was cut
}

// -- through the C ABI --------------------------------------------------------

TEST_CASE("a host converts a sculpt into a layer it can boolean against") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id vox = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "sculpt", 0.05f, &vox, &grid) == CLAY_OK);
    REQUIRE(grid != nullptr);

    // Two colours, so the per-palette-entry conversion has something to carry.
    int32_t red = 0, blue = 0;
    float red_rgb[3] = {1, 0, 0}, blue_rgb[3] = {0, 0, 1};
    REQUIRE(clay_voxel_palette_add(grid, red_rgb, &red) == CLAY_OK);
    REQUIRE(clay_voxel_palette_add(grid, blue_rgb, &blue) == CLAY_OK);
    int32_t lo[3] = {-6, -6, -6}, mid_hi[3] = {-1, 6, 6};
    int32_t mid_lo[3] = {0, -6, -6}, hi[3] = {6, 6, 6};
    REQUIRE(clay_voxel_fill_box(grid, lo, mid_hi, red) == CLAY_OK);
    REQUIRE(clay_voxel_fill_box(grid, mid_lo, hi, blue) == CLAY_OK);

    clay_layer_id converted = 0;
    REQUIRE(clay_voxel_to_layer(doc, grid, "converted", 0, &converted) == CLAY_OK);
    CHECK(converted != vox);

    // One item per palette entry the grid carried, each keeping its colour.
    size_t nodes = 0;
    REQUIRE(clay_layer_node_count(doc, converted, &nodes) == CLAY_OK);
    CHECK(nodes == 2);

    // And it evaluates as a solid: the point deep inside the red half is
    // inside the converted layer too.
    float points[3] = {-0.2f, 0, 0};
    float distance = 0;
    REQUIRE(clay_layer_eval_points(doc, converted, nullptr, points, 1, &distance, nullptr) ==
            CLAY_OK);
    CHECK(distance < 0.0f);

    // The grid is untouched — the conversion is non-destructive, which is what
    // lets a host offer "go back".
    size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    CHECK(occupied > 0);

    clay_document_destroy(doc);
}

TEST_CASE("converting an empty grid refuses rather than making an empty layer") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id vox = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "sculpt", 0.05f, &vox, &grid) == CLAY_OK);

    clay_layer_id converted = 999;
    CHECK(clay_voxel_to_layer(doc, grid, "converted", 0, &converted) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // And the document did not grow a layer on the way out.
    size_t layers = 0;
    REQUIRE(clay_document_layer_count(doc, &layers) == CLAY_OK);
    CHECK(layers == 1);

    CHECK(clay_voxel_to_layer(doc, grid, "converted", 99, &converted) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_document_destroy(doc);
}
