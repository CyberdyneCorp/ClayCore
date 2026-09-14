#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
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
    p.struct_size = sizeof(p);
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
    out.rotate_to_azimuth = p.rotate_to_azimuth != 0;
    out.velocity_response.size = p.velocity_size;
    out.velocity_response.strength = p.velocity_strength;
    out.velocity_response.reference = p.velocity_reference;
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
    back.struct_size = sizeof(back);
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

namespace {

// The issue-61 fixture: one stamp on a unit sphere at the pole, measured as
// the surface's displacement along +Z. Returns the displacement and, through
// out_nodes, how many nodes the stroke authored.
float pole_displacement(std::int32_t op, float strength, std::int32_t accumulation,
                        std::size_t* out_nodes = nullptr) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);

    clay_item_desc base{};
    base.struct_size = sizeof base;
    base.prim = CLAY_PRIM_SPHERE;
    base.params[0] = 1.0f;
    base.rotation[3] = 1.0f;
    base.scale = 1.0f;
    base.op = CLAY_OP_ADD;
    REQUIRE(clay_add_item(doc, layer, &base, nullptr) == CLAY_OK);

    const float r[1] = {0.18f};
    clay_item* stamp = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(stamp != nullptr);
    REQUIRE(clay_item_set_op(stamp, op) == CLAY_OK);
    REQUIRE(clay_item_set_blend(stamp, CLAY_BLEND_QUADRATIC, 0.18f) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(stamp, 0.18f) == CLAY_OK);

    clay_stroke_preset p = defaults();
    p.radius = 0.18f;
    p.strength = strength;
    p.accumulation = accumulation;

    const float sample[5] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f};  // the pole, full pressure
    std::size_t count = 0;
    REQUIRE(clay_layer_apply_stroke(doc, layer, sample, 1, &p, stamp, nullptr, nullptr, &count) ==
            CLAY_OK);
    if (out_nodes) *out_nodes = count;
    clay_item_destroy(stamp);

    const float origin[3] = {0.0f, 0.0f, 3.0f};
    const float dir[3] = {0.0f, 0.0f, -1.0f};
    std::int32_t hit = 0;
    float t = 0.0f, pos[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
    REQUIRE(clay_raycast(doc, origin, dir, &hit, &t, pos, normal) == CLAY_OK);
    REQUIRE(hit == 1);
    clay_document_destroy(doc);
    return pos[2] - 1.0f;
}

}  // namespace

// Issue #61: CLAY_OP_ADD deposited a full stamp at every strength, zero
// included, while CLAY_OP_RELIEF scaled correctly.
TEST_CASE("c stroke: strength scales what a stamp deposits") {
    const std::int32_t buildup = CLAY_ACCUMULATION_BUILDUP;

    SUBCASE("add honours strength: nothing at zero, monotonic, exact at one") {
        std::size_t authored = ~std::size_t(0);
        const float d0 = pole_displacement(CLAY_OP_ADD, 0.0f, buildup, &authored);
        CHECK(std::fabs(d0) < 1e-3f);
        CHECK(authored == 0);  // a zero-strength stamp authors no node at all

        const float d10 = pole_displacement(CLAY_OP_ADD, 0.1f, buildup);
        const float d50 = pole_displacement(CLAY_OP_ADD, 0.5f, buildup);
        const float d100 = pole_displacement(CLAY_OP_ADD, 1.0f, buildup);
        INFO("add displaces " << d10 << " / " << d50 << " / " << d100);
        CHECK(d10 > d0 + 1e-3f);
        CHECK(d50 > d10 + 1e-3f);
        CHECK(d100 > d50 + 1e-3f);

        // Full strength is the item exactly as authored: the same edit placed
        // by hand at the stamp's transform lands the surface in the same spot.
        clay_document* doc = clay_document_create();
        REQUIRE(doc != nullptr);
        clay_layer_id layer = 0;
        REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
        clay_item_desc base{};
        base.struct_size = sizeof base;
        base.prim = CLAY_PRIM_SPHERE;
        base.params[0] = 1.0f;
        base.rotation[3] = 1.0f;
        base.scale = 1.0f;
        base.op = CLAY_OP_ADD;
        REQUIRE(clay_add_item(doc, layer, &base, nullptr) == CLAY_OK);
        clay_item_desc hand{};
        hand.struct_size = sizeof hand;
        hand.prim = CLAY_PRIM_SPHERE;
        hand.params[0] = 0.18f;
        hand.position[2] = 1.0f;
        hand.rotation[3] = 1.0f;
        hand.scale = 0.18f;  // the stamp's radius, exactly as stamps scale
        hand.op = CLAY_OP_ADD;
        hand.blend = CLAY_BLEND_QUADRATIC;
        hand.blend_k = 0.18f;
        hand.rounding = 0.18f;
        REQUIRE(clay_add_item(doc, layer, &hand, nullptr) == CLAY_OK);
        const float origin[3] = {0.0f, 0.0f, 3.0f};
        const float dir[3] = {0.0f, 0.0f, -1.0f};
        std::int32_t hit = 0;
        float t = 0.0f, pos[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
        REQUIRE(clay_raycast(doc, origin, dir, &hit, &t, pos, normal) == CLAY_OK);
        REQUIRE(hit == 1);
        CHECK(d100 == doctest::Approx(pos[2] - 1.0f).epsilon(1e-5));
        clay_document_destroy(doc);
    }

    SUBCASE("relief still scales as it did") {
        const float d0 = pole_displacement(CLAY_OP_RELIEF, 0.0f, buildup);
        const float d10 = pole_displacement(CLAY_OP_RELIEF, 0.1f, buildup);
        const float d50 = pole_displacement(CLAY_OP_RELIEF, 0.5f, buildup);
        const float d100 = pole_displacement(CLAY_OP_RELIEF, 1.0f, buildup);
        INFO("relief displaces " << d0 << " / " << d10 << " / " << d50 << " / " << d100);
        CHECK(std::fabs(d0) < 1e-3f);
        CHECK(d10 > d0 + 1e-3f);
        CHECK(d50 > d10 + 1e-3f);
        CHECK(d100 > d50 + 1e-3f);
    }

    SUBCASE("a clamped add stroke keeps its deposit") {
        // Overlapping unions do not add up, so the clamped division that keeps
        // a relief stroke at its strength must not shrink an add stamp.
        const float clamped = pole_displacement(CLAY_OP_ADD, 1.0f, CLAY_ACCUMULATION_CLAMPED);
        const float built = pole_displacement(CLAY_OP_ADD, 1.0f, buildup);
        CHECK(clamped == doctest::Approx(built).epsilon(1e-6));
    }
}

// -- the barrel and the speed response (issue #530) ---------------------------
//
// `brush::StrokePreset` carried rotate_to_azimuth and the velocity response
// from preset schema version 2; clay_stroke_preset did not carry either, so the
// C ABI could describe neither a rake nor a speed-driven brush, and
// clay_brush_preset_by_name("Rake") handed back a preset with the one field
// that makes it a rake dropped on the way out. These are the two controls that
// were genuinely missing — every other name in the C++ struct already crossed,
// `pressure` as the three flattened pressure_* fields.

namespace {

std::vector<clay_stroke_sample_full> wide_line(float length, float step, float azimuth,
                                               float velocity) {
    std::vector<clay_stroke_sample_full> out;
    for (float d = 0.0f; d <= length + 1e-5f; d += step) {
        clay_stroke_sample_full s{};
        s.position[0] = d;
        s.pressure = 1.0f;
        s.tilt = 0.4f;
        s.azimuth = azimuth;
        s.velocity = velocity;
        out.push_back(s);
    }
    return out;
}

// Where a stamp's own +X ends up, which is what "the stamp faces this way"
// means: `resolve_stroke` builds the rotation with align_x_to.
kernel::cfloat3 facing(const clay_stamp& s) {
    const math::Quat q{s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3]};
    return q.rotate(kernel::cf3(1, 0, 0));
}

std::vector<clay_stamp> resolve_wide(const std::vector<clay_stroke_sample_full>& samples,
                                     const clay_stroke_preset& p) {
    std::size_t count = 0;
    REQUIRE(clay_stroke_resolve_full(samples.data(), samples.size(), &p, nullptr, &count) ==
            CLAY_OK);
    REQUIRE(count > 0);
    std::vector<clay_stamp> stamps(count);
    std::size_t capacity = count;
    REQUIRE(clay_stroke_resolve_full(samples.data(), samples.size(), &p, stamps.data(),
                                     &capacity) == CLAY_OK);
    REQUIRE(capacity == count);
    return stamps;
}

}  // namespace

TEST_CASE("c stroke: the barrel crosses the ABI, and wins over the path") {
    const float azimuth = 1.1f;  // radians, in the surface plane; 0 is +x
    const std::vector<clay_stroke_sample_full> samples = wide_line(1.0f, 0.05f, azimuth, 0.0f);
    // A path along +X, so "follows the path" and "follows the barrel" are
    // different answers and the test cannot pass by accident.
    const kernel::cfloat3 barrel = kernel::cf3(std::cos(azimuth), 0.0f, std::sin(azimuth));

    clay_stroke_preset along = defaults();
    along.radius = 0.1f;
    along.spacing = 0.5f;
    along.rotate_along_stroke = 1;

    clay_stroke_preset barrel_only = along;
    barrel_only.rotate_along_stroke = 0;
    barrel_only.rotate_to_azimuth = 1;

    clay_stroke_preset both = along;
    both.rotate_to_azimuth = 1;

    const std::vector<clay_stamp> a = resolve_wide(samples, along);
    const std::vector<clay_stamp> b = resolve_wide(samples, barrel_only);
    const std::vector<clay_stamp> c = resolve_wide(samples, both);
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());

    SUBCASE("rotate_along_stroke still follows the path") {
        const kernel::cfloat3 f = facing(a[1]);
        CHECK(f.x == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(f.z == doctest::Approx(0.0f).epsilon(1e-4));
    }

    SUBCASE("rotate_to_azimuth follows the stylus barrel") {
        const kernel::cfloat3 f = facing(b[1]);
        INFO("faces " << f.x << ", " << f.y << ", " << f.z << " for azimuth " << azimuth);
        CHECK(f.x == doctest::Approx(barrel.x).epsilon(1e-4));
        CHECK(f.z == doctest::Approx(barrel.z).epsilon(1e-4));
        // ...and that is not where the path pointed, so the field did something.
        CHECK(std::fabs(f.x - 1.0f) > 0.1f);
    }

    SUBCASE("both set: the barrel wins, because a stamp cannot face two ways") {
        for (std::size_t i = 0; i < c.size(); ++i) {
            CAPTURE(i);
            for (int k = 0; k < 4; ++k)
                CHECK(c[i].rotation[k] == doctest::Approx(b[i].rotation[k]).epsilon(1e-5));
        }
    }

    SUBCASE("the C boundary agrees with the engine stamp for stamp") {
        std::vector<brush::StrokeSample> engine;
        for (const clay_stroke_sample_full& s : samples) {
            brush::StrokeSample e;
            e.position = kernel::cf3(s.position[0], s.position[1], s.position[2]);
            e.pressure = s.pressure;
            e.tilt = s.tilt;
            e.azimuth = s.azimuth;
            e.velocity = s.velocity;
            engine.push_back(e);
        }
        const std::vector<brush::Stamp> expected =
            brush::resolve_stroke(engine, engine_preset(both));
        REQUIRE(expected.size() == c.size());
        for (std::size_t i = 0; i < c.size(); ++i) {
            CAPTURE(i);
            CHECK(c[i].rotation[0] == doctest::Approx(expected[i].rotation.x));
            CHECK(c[i].rotation[1] == doctest::Approx(expected[i].rotation.y));
            CHECK(c[i].rotation[2] == doctest::Approx(expected[i].rotation.z));
            CHECK(c[i].rotation[3] == doctest::Approx(expected[i].rotation.w));
        }
    }
}

TEST_CASE("c stroke: on the flat sample packing the barrel is inert, not wrong") {
    // The flat count*5 packing reports no azimuth, so azimuth is 0 — which
    // points a stamp at +X and is the IDENTITY rotation. Worth pinning because
    // the header claims it: a host applying a barrel preset through
    // clay_layer_apply_stroke gets the stroke it got before, not a stroke
    // rotated into a wall.
    clay_stroke_preset p = defaults();
    p.radius = 0.1f;
    p.spacing = 0.5f;
    p.rotate_to_azimuth = 1;

    const std::vector<float> flat = packed(line(1.0f, 0.05f));
    std::size_t count = 0;
    REQUIRE(clay_stroke_resolve(flat.data(), flat.size() / 5, &p, nullptr, &count) == CLAY_OK);
    std::vector<clay_stamp> stamps(count);
    std::size_t capacity = count;
    REQUIRE(clay_stroke_resolve(flat.data(), flat.size() / 5, &p, stamps.data(), &capacity) ==
            CLAY_OK);
    for (std::size_t i = 0; i < count; ++i) {
        CAPTURE(i);
        CHECK(stamps[i].rotation[0] == doctest::Approx(0.0f));
        CHECK(stamps[i].rotation[1] == doctest::Approx(0.0f));
        CHECK(stamps[i].rotation[2] == doctest::Approx(0.0f));
        CHECK(stamps[i].rotation[3] == doctest::Approx(1.0f));
    }
}

TEST_CASE("c stroke: the speed response crosses the ABI, signed") {
    // Reference 2.0 with samples at 2.0 units per second: the response is fully
    // applied, so the radius moves by the whole of velocity_size.
    const std::vector<clay_stroke_sample_full> fast = wide_line(1.0f, 0.05f, 0.0f, 2.0f);
    const std::vector<clay_stroke_sample_full> still = wide_line(1.0f, 0.05f, 0.0f, 0.0f);

    clay_stroke_preset off = defaults();
    off.radius = 0.1f;
    off.spacing = 0.5f;

    clay_stroke_preset wider = off;
    wider.velocity_size = 0.5f;
    wider.velocity_reference = 2.0f;

    clay_stroke_preset thinner = off;
    thinner.velocity_size = -0.5f;
    thinner.velocity_reference = 2.0f;

    const float base = resolve_wide(fast, off)[1].radius;
    const float grown = resolve_wide(fast, wider)[1].radius;
    const float shrunk = resolve_wide(fast, thinner)[1].radius;
    INFO("radius off " << base << " wider " << grown << " thinner " << shrunk);
    CHECK(base == doctest::Approx(0.1f));
    CHECK(grown == doctest::Approx(0.15f));
    CHECK(shrunk == doctest::Approx(0.05f));

    SUBCASE("a stroke that is not moving is unchanged by either sign") {
        CHECK(resolve_wide(still, wider)[1].radius == doctest::Approx(base));
        CHECK(resolve_wide(still, thinner)[1].radius == doctest::Approx(base));
    }

    SUBCASE("the strength channel moves the deposit") {
        clay_stroke_preset softer = off;
        softer.strength = 0.5f;
        softer.velocity_strength = -0.5f;
        softer.velocity_reference = 2.0f;
        const std::vector<clay_stamp> s = resolve_wide(fast, softer);
        CHECK(s[1].strength == doctest::Approx(0.25f));
    }

    SUBCASE("a response with no reference speed is refused, not silently inert") {
        clay_stroke_preset no_reference = off;
        no_reference.velocity_size = 0.5f;
        no_reference.velocity_reference = 0.0f;
        std::size_t count = 0;
        CHECK(clay_stroke_resolve_full(fast.data(), fast.size(), &no_reference, nullptr, &count) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        no_reference.velocity_size = 0.0f;
        no_reference.velocity_strength = -0.25f;
        CHECK(clay_stroke_resolve_full(fast.data(), fast.size(), &no_reference, nullptr, &count) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // Neither channel asking: the zero reference an older caller leaves
        // behind is legal and means "speed changes nothing".
        no_reference.velocity_strength = 0.0f;
        CHECK(clay_stroke_resolve_full(fast.data(), fast.size(), &no_reference, nullptr, &count) ==
              CLAY_OK);
    }
}

TEST_CASE("c stroke: the appended controls are behind struct_size") {
    // The whole claim of an appended field: a host compiled against the shorter
    // layout declares the shorter size and resolves the stroke it always did,
    // whatever happens to sit in the memory past what it declared.
    const std::vector<clay_stroke_sample_full> samples = wide_line(1.0f, 0.05f, 1.1f, 2.0f);

    clay_stroke_preset before = defaults();
    before.radius = 0.1f;
    before.spacing = 0.5f;
    before.velocity_reference = 0.0f;  // the struct's own zero, not the call's 1.0
    const std::vector<clay_stamp> expected = resolve_wide(samples, before);

    clay_stroke_preset old_host = before;
    old_host.struct_size =
        static_cast<std::uint32_t>(offsetof(clay_stroke_preset, rotate_to_azimuth));
    REQUIRE(old_host.struct_size < sizeof(clay_stroke_preset));
    // Everything past what it declared asks loudly for both controls.
    old_host.rotate_to_azimuth = 1;
    old_host.velocity_size = 4.0f;
    old_host.velocity_strength = -1.0f;
    old_host.velocity_reference = 0.5f;

    const std::vector<clay_stamp> got = resolve_wide(samples, old_host);
    REQUIRE(got.size() == expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i].radius == doctest::Approx(expected[i].radius));
        CHECK(got[i].strength == doctest::Approx(expected[i].strength));
        for (int k = 0; k < 4; ++k)
            CHECK(got[i].rotation[k] == doctest::Approx(expected[i].rotation[k]));
    }

    SUBCASE("and the defaults call supplies the reference the struct's zero cannot") {
        const clay_stroke_preset d = defaults();
        CHECK(d.rotate_to_azimuth == 0);
        CHECK(d.velocity_size == doctest::Approx(0.0f));
        CHECK(d.velocity_strength == doctest::Approx(0.0f));
        CHECK(d.velocity_reference == doctest::Approx(1.0f));
    }
}

TEST_CASE("c stroke: a preset round trip keeps the barrel and the speed response") {
    // The C round trip used to lose both: serialize read them off a struct that
    // did not have them, so a schema-v2 preset came back as a v1 one in all but
    // its version byte.
    clay_stroke_preset p = defaults();
    p.radius = 0.3f;
    p.rotate_along_stroke = 1;
    p.rotate_to_azimuth = 1;
    p.velocity_size = -0.25f;
    p.velocity_strength = 0.75f;
    p.velocity_reference = 3.5f;

    std::size_t size = 0;
    REQUIRE(clay_stroke_preset_serialize(&p, nullptr, &size) == CLAY_OK);
    std::vector<std::uint8_t> bytes(size);
    std::size_t capacity = size;
    REQUIRE(clay_stroke_preset_serialize(&p, bytes.data(), &capacity) == CLAY_OK);

    clay_stroke_preset back{};
    back.struct_size = sizeof(back);
    REQUIRE(clay_stroke_preset_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
    CHECK(back.rotate_to_azimuth == 1);
    CHECK(back.rotate_along_stroke == 1);
    CHECK(back.velocity_size == doctest::Approx(-0.25f));
    CHECK(back.velocity_strength == doctest::Approx(0.75f));
    CHECK(back.velocity_reference == doctest::Approx(3.5f));
}
