#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/brush/stroke.h"

// The C ABI stroke surface (c-abi spec: the stroke engine). Same standard as
// the voxel and mask cases: each stroke is resolved twice, once through the C
// boundary and once on the engine types the way the Python bindings do, and
// the two have to agree stamp for stamp. Jitter is a hash, so "agree" means
// exactly, not approximately.

using namespace clay;

namespace {

std::vector<float> packed(const std::vector<brush::StrokeSample>& samples) {
    std::vector<float> out;
    out.reserve(samples.size() * 5);
    for (const brush::StrokeSample& s : samples) {
        out.push_back(s.position.x);
        out.push_back(s.position.y);
        out.push_back(s.position.z);
        out.push_back(s.pressure);
        out.push_back(s.tilt);
    }
    return out;
}

std::vector<brush::StrokeSample> line(float length, float step) {
    std::vector<brush::StrokeSample> out;
    for (float d = 0.0f; d <= length + 1e-5f; d += step) {
        brush::StrokeSample s;
        s.position = kernel::cf3(d, 0, 0);
        out.push_back(s);
    }
    return out;
}

clay_stroke_preset defaults() {
    clay_stroke_preset p;
    REQUIRE(clay_stroke_preset_defaults(&p) == CLAY_OK);
    return p;
}

brush::StrokePreset engine_preset(const clay_stroke_preset& p) {
    brush::StrokePreset out;
    out.radius = p.radius;
    out.spacing = p.spacing;
    out.strength = p.strength;
    out.pressure.size = p.pressure_size;
    out.pressure.strength = p.pressure_strength;
    out.pressure.curve = p.pressure_curve;
    out.jitter_position = p.jitter_position;
    out.jitter_size = p.jitter_size;
    out.jitter_rotation = p.jitter_rotation;
    out.seed = p.seed;
    out.rotate_along_stroke = p.rotate_along_stroke != 0;
    out.taper_start = p.taper_start;
    out.taper_end = p.taper_end;
    out.steady = p.steady;
    out.accumulation = static_cast<brush::Accumulation>(p.accumulation);
    return out;
}

}  // namespace

TEST_CASE("c stroke: defaults are what the engine defaults are") {
    clay_stroke_preset p = defaults();
    CHECK(p.struct_size == sizeof(clay_stroke_preset));
    brush::StrokePreset d;
    CHECK(p.radius == doctest::Approx(d.radius));
    CHECK(p.spacing == doctest::Approx(d.spacing));
    CHECK(p.pressure_strength == doctest::Approx(d.pressure.strength));
    CHECK(p.accumulation == static_cast<std::int32_t>(d.accumulation));
    CHECK(clay_stroke_preset_defaults(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c stroke: resolution matches the engine, jitter included") {
    clay_stroke_preset p = defaults();
    p.radius = 0.2f;
    p.spacing = 0.4f;
    p.jitter_position = 0.4f;
    p.jitter_size = 0.25f;
    p.seed = 909;
    p.taper_start = 0.1f;

    std::vector<brush::StrokeSample> samples = line(2.0f, 0.1f);
    std::vector<float> flat = packed(samples);

    std::size_t count = 0;
    REQUIRE(clay_stroke_resolve(flat.data(), samples.size(), &p, nullptr, &count) == CLAY_OK);
    REQUIRE(count > 3);

    std::vector<clay_stamp> stamps(count);
    std::size_t capacity = count;
    REQUIRE(clay_stroke_resolve(flat.data(), samples.size(), &p, stamps.data(), &capacity) ==
            CLAY_OK);
    CHECK(capacity == count);

    std::vector<brush::Stamp> expected = brush::resolve_stroke(samples, engine_preset(p));
    REQUIRE(expected.size() == count);
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(stamps[i].position[0] == doctest::Approx(expected[i].position.x));
        CHECK(stamps[i].position[1] == doctest::Approx(expected[i].position.y));
        CHECK(stamps[i].radius == doctest::Approx(expected[i].radius));
        CHECK(stamps[i].strength == doctest::Approx(expected[i].strength));
        CHECK(stamps[i].along == doctest::Approx(expected[i].along));
    }

    SUBCASE("a short buffer reports what it needed") {
        std::vector<clay_stamp> small(1);
        std::size_t one = 1;
        CHECK(clay_stroke_resolve(flat.data(), samples.size(), &p, small.data(), &one) ==
              CLAY_ERROR_BUFFER_TOO_SMALL);
        CHECK(one == count);
    }
}

TEST_CASE("c stroke: presets round trip and refuse a newer schema") {
    clay_stroke_preset p = defaults();
    p.radius = 0.42f;
    p.spacing = 0.31f;
    p.jitter_rotation = 1.1f;
    p.seed = 77;
    p.rotate_along_stroke = 1;
    p.accumulation = CLAY_ACCUMULATION_CLAMPED;

    std::size_t size = 0;
    REQUIRE(clay_stroke_preset_serialize(&p, nullptr, &size) == CLAY_OK);
    REQUIRE(size > 0);
    std::vector<std::uint8_t> bytes(size);
    std::size_t capacity = size;
    REQUIRE(clay_stroke_preset_serialize(&p, bytes.data(), &capacity) == CLAY_OK);
    CHECK(capacity == size);

    clay_stroke_preset back;
    REQUIRE(clay_stroke_preset_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
    CHECK(back.struct_size == sizeof(clay_stroke_preset));
    CHECK(back.radius == doctest::Approx(p.radius));
    CHECK(back.spacing == doctest::Approx(p.spacing));
    CHECK(back.jitter_rotation == doctest::Approx(p.jitter_rotation));
    CHECK(back.seed == p.seed);
    CHECK(back.rotate_along_stroke == 1);
    CHECK(back.accumulation == CLAY_ACCUMULATION_CLAMPED);

    SUBCASE("a newer schema version is refused") {
        std::vector<std::uint8_t> newer = bytes;
        newer[0] = static_cast<std::uint8_t>(clay_stroke_preset_version() + 1);
        clay_stroke_preset out;
        CHECK(clay_stroke_preset_deserialize(newer.data(), newer.size(), &out) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("a short buffer reports what it needed") {
        std::vector<std::uint8_t> small(1);
        std::size_t one = 1;
        CHECK(clay_stroke_preset_serialize(&p, small.data(), &one) == CLAY_ERROR_BUFFER_TOO_SMALL);
        CHECK(one == size);
    }
    SUBCASE("nothing is not a preset") {
        clay_stroke_preset out;
        CHECK(clay_stroke_preset_deserialize(nullptr, 0, &out) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_stroke_preset_deserialize(bytes.data(), 1, &out) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("c stroke: a preset descriptor is validated, not clamped") {
    clay_stroke_preset p = defaults();
    std::vector<float> flat = packed(line(1.0f, 0.1f));
    std::size_t count = 0;

    p.radius = 0.0f;
    CHECK(clay_stroke_resolve(flat.data(), 11, &p, nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    p = defaults();
    p.spacing = -1.0f;
    CHECK(clay_stroke_resolve(flat.data(), 11, &p, nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    p = defaults();
    p.accumulation = 7;
    CHECK(clay_stroke_resolve(flat.data(), 11, &p, nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    p = defaults();
    p.struct_size = 4;  // below the original layout
    CHECK(clay_stroke_resolve(flat.data(), 11, &p, nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);

    p = defaults();
    CHECK(clay_stroke_resolve(flat.data(), 11, nullptr, nullptr, &count) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_stroke_resolve(nullptr, 11, &p, nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_stroke_resolve(flat.data(), 11, &p, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c stroke: applied to a voxel grid, and gated by a mask") {
    clay_stroke_preset p = defaults();
    p.radius = 0.15f;
    p.spacing = 0.5f;
    std::vector<brush::StrokeSample> samples;
    for (int i = 0; i <= 60; ++i) {
        brush::StrokeSample s;
        s.position = kernel::cf3(-1.5f + i * 0.05f, 0, 0);
        samples.push_back(s);
    }
    std::vector<float> flat = packed(samples);

    clay_voxel_grid* grid = clay_voxel_grid_create(0.05f);
    REQUIRE(grid != nullptr);
    const float white[3] = {1.0f, 1.0f, 1.0f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(grid, white, &index) == CLAY_OK);

    std::size_t applied = 0;
    REQUIRE(clay_voxel_apply_stroke(grid, flat.data(), samples.size(), &p, index,
                                    CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, nullptr,
                                    &applied) == CLAY_OK);
    CHECK(applied > 5);
    std::size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    CHECK(occupied > 0);

    SUBCASE("a frozen half receives nothing") {
        clay_mask* mask = clay_mask_create(0.05f);
        REQUIRE(mask != nullptr);
        for (int x = 0; x < 40; ++x)
            for (int y = -20; y <= 20; ++y)
                for (int z = -20; z <= 20; ++z) {
                    std::int32_t cell[3] = {x, y, z};
                    REQUIRE(clay_mask_set(mask, cell, 1.0f) == CLAY_OK);
                }

        clay_voxel_grid* gated = clay_voxel_grid_create(0.05f);
        REQUIRE(gated != nullptr);
        std::int32_t gi = 0;
        REQUIRE(clay_voxel_palette_add(gated, white, &gi) == CLAY_OK);
        std::size_t gated_count = 0;
        REQUIRE(clay_voxel_apply_stroke(gated, flat.data(), samples.size(), &p, gi,
                                        CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, mask,
                                        &gated_count) == CLAY_OK);
        CHECK(gated_count < applied);
        CHECK(gated_count > 0);
        clay_mask_destroy(mask);
        clay_voxel_grid_destroy(gated);
    }

    SUBCASE("an unknown shape or palette index is refused") {
        CHECK(clay_voxel_apply_stroke(grid, flat.data(), samples.size(), &p, index, 99,
                                      CLAY_BRUSH_FALLOFF_SMOOTH, nullptr, &applied) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_voxel_apply_stroke(grid, flat.data(), samples.size(), &p, 999,
                                      CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, nullptr,
                                      &applied) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    clay_voxel_grid_destroy(grid);
}

TEST_CASE("c stroke: applied to a layer as one undo step") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);

    const float radius[1] = {1.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    REQUIRE(item != nullptr);

    clay_stroke_preset p = defaults();
    p.radius = 0.2f;
    p.spacing = 0.5f;
    std::vector<float> flat = packed(line(2.0f, 0.1f));
    const std::size_t sample_count = flat.size() / 5;

    std::size_t stamps = 0;
    REQUIRE(clay_stroke_resolve(flat.data(), sample_count, &p, nullptr, &stamps) == CLAY_OK);
    REQUIRE(stamps > 3);

    std::vector<clay_node_id> nodes(stamps);
    std::size_t count = stamps;
    REQUIRE(clay_layer_apply_stroke(doc, layer, flat.data(), sample_count, &p, item, nullptr,
                                    nodes.data(), &count) == CLAY_OK);
    CHECK(count == stamps);
    for (clay_node_id id : nodes) CHECK(id != 0);

    // One step for the whole stroke, not one per stamp.
    std::int32_t enabled = 0;
    std::size_t undo_depth = 0, redo_depth = 0;
    REQUIRE(clay_document_undo_state(doc, &enabled, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(undo_depth == 1);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_undo_state(doc, &enabled, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(undo_depth == 0);
    CHECK(redo_depth == 1);

    SUBCASE("the ids are optional, and a short buffer still applies once") {
        std::size_t none = 0;
        REQUIRE(clay_layer_apply_stroke(doc, layer, flat.data(), sample_count, &p, item, nullptr,
                                        nullptr, &none) == CLAY_OK);
        CHECK(none == stamps);

        clay_node_id one_id = 0;
        std::size_t one = 1;
        REQUIRE(clay_layer_apply_stroke(doc, layer, flat.data(), sample_count, &p, item, nullptr,
                                        &one_id, &one) == CLAY_OK);
        CHECK(one == stamps);  // the true total, not the capacity
        CHECK(one_id != 0);
    }

    SUBCASE("an unknown layer is refused before anything is applied") {
        std::size_t none = 0;
        CHECK(clay_layer_apply_stroke(doc, 999, flat.data(), sample_count, &p, item, nullptr,
                                      nullptr, &none) == CLAY_ERROR_NOT_FOUND);
        CHECK(clay_layer_apply_stroke(nullptr, layer, flat.data(), sample_count, &p, item, nullptr,
                                      nullptr, &none) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_apply_stroke(doc, layer, flat.data(), sample_count, &p, nullptr, nullptr,
                                      nullptr, &none) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    clay_item_destroy(item);
    clay_document_destroy(doc);
}
