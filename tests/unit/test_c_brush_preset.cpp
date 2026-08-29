// The brush preset across the C ABI (add-shared-brush-kernels 7.1).
//
// What this gates is the descriptor discipline rather than the brush maths:
// `struct_size` on every descriptor, a bounded fill that never writes past what
// the caller declared, bytes rather than a path, and a newer schema refused
// rather than read as a prefix.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

TEST_CASE("c brush preset: the library crosses by name and by index") {
    const size_t count = clay_brush_preset_library_count();
    REQUIRE(count >= 19);

    for (size_t i = 0; i < count; ++i) {
        clay_brush_preset p{};
        p.struct_size = sizeof(p);
        CAPTURE(i);
        REQUIRE(clay_brush_preset_library_at(i, &p) == CLAY_OK);
        CHECK(p.name[0] != '\0');
        // The name is NUL-terminated within the array, whatever the source was.
        bool terminated = false;
        for (size_t k = 0; k < CLAY_BRUSH_PRESET_NAME_MAX; ++k)
            if (p.name[k] == '\0') terminated = true;
        CHECK(terminated);
        CHECK(p.brush.radius > 0.0f);
        CHECK(p.stroke.spacing > 0.0f);

        // ...and the same preset comes back by name.
        clay_brush_preset by_name{};
        by_name.struct_size = sizeof(by_name);
        REQUIRE(clay_brush_preset_by_name(p.name, &by_name) == CLAY_OK);
        CHECK(by_name.brush.verb == p.brush.verb);
        CHECK(by_name.model.kernel == p.model.kernel);
    }

    clay_brush_preset missing{};
    missing.struct_size = sizeof(missing);
    CHECK(clay_brush_preset_library_at(count, &missing) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_brush_preset_by_name("no such brush", &missing) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_brush_preset_by_name(nullptr, &missing) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c brush preset: draw and inflate are one kernel under two frames") {
    clay_brush_model draw{}, inflate{};
    draw.struct_size = sizeof(draw);
    inflate.struct_size = sizeof(inflate);
    REQUIRE(clay_brush_model_of(CLAY_MESH_BRUSH_DRAW, &draw) == CLAY_OK);
    REQUIRE(clay_brush_model_of(CLAY_MESH_BRUSH_INFLATE, &inflate) == CLAY_OK);

    CHECK(draw.kernel == CLAY_BRUSH_KERNEL_DISPLACE);
    CHECK(inflate.kernel == CLAY_BRUSH_KERNEL_DISPLACE);
    CHECK(draw.frame == CLAY_BRUSH_FRAME_REGION_NORMAL);
    CHECK(inflate.frame == CLAY_BRUSH_FRAME_VERTEX_NORMAL);

    // A verb outside the vocabulary is refused rather than decoded.
    clay_brush_model bad{};
    bad.struct_size = sizeof(bad);
    CHECK(clay_brush_model_of(999, &bad) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c brush preset: bytes round-trip through the size query") {
    clay_brush_preset p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_brush_preset_by_name("hPolish", &p) == CLAY_OK);

    // The size query: NULL buffer asks how many bytes.
    size_t needed = 0;
    REQUIRE(clay_brush_preset_serialize(&p, nullptr, &needed) == CLAY_OK);
    REQUIRE(needed > 0);

    std::vector<uint8_t> bytes(needed);
    size_t written = needed;
    REQUIRE(clay_brush_preset_serialize(&p, bytes.data(), &written) == CLAY_OK);
    CHECK(written == needed);

    clay_brush_preset back{};
    back.struct_size = sizeof(back);
    REQUIRE(clay_brush_preset_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
    CHECK(std::string(back.name) == "hPolish");
    CHECK(back.brush.verb == p.brush.verb);
    CHECK(back.brush.polish_angle == p.brush.polish_angle);
    CHECK(back.model.kernel == p.model.kernel);
    CHECK(back.stroke.spacing == p.stroke.spacing);
}

TEST_CASE("c brush preset: a newer schema is refused and nothing is written") {
    clay_brush_preset p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_brush_preset_by_name("Standard", &p) == CLAY_OK);

    size_t needed = 0;
    REQUIRE(clay_brush_preset_serialize(&p, nullptr, &needed) == CLAY_OK);
    std::vector<uint8_t> bytes(needed);
    size_t written = needed;
    REQUIRE(clay_brush_preset_serialize(&p, bytes.data(), &written) == CLAY_OK);

    bytes[4] = static_cast<uint8_t>(clay_brush_preset_version() + 1);
    clay_brush_preset back{};
    back.struct_size = sizeof(back);
    const char* untouched = back.name;
    CHECK(clay_brush_preset_deserialize(bytes.data(), bytes.size(), &back) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // No partially populated preset: the output is exactly as it was.
    CHECK(untouched[0] == '\0');

    CHECK(clay_brush_preset_deserialize(nullptr, 0, &back) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c brush preset: an older brush descriptor is honoured and not overrun") {
    // THE PREFIX RULE, tested where it actually applies. `clay_brush_preset` is
    // new, so its original layout IS its whole layout and a shorter one is a
    // caller error rather than an older host — which is what the first draft of
    // this test got wrong. The descriptor that genuinely GAINED fields is
    // `clay_mesh_brush_desc`, which grew the automask block.
    clay_mesh_brush_desc older{};
    older.struct_size = offsetof(clay_mesh_brush_desc, automask_factors);
    REQUIRE(clay_mesh_brush_defaults(&older) == CLAY_OK);
    // The fields it declared are filled...
    CHECK(older.radius > 0.0f);
    CHECK(older.verb == CLAY_MESH_BRUSH_DRAW);
    // ...and not one byte past them: a host compiled before automasking existed
    // sees exactly what it saw.
    CHECK(older.automask_factors == 0u);
    CHECK(older.automask_boundary_rings == 0);

    // A preset declaring less than its whole layout is refused, because there
    // is no older layout for it to be.
    clay_brush_preset p{};
    p.struct_size = offsetof(clay_brush_preset, model);
    CHECK(clay_brush_preset_by_name("Clay", &p) != CLAY_OK);
}

TEST_CASE("c brush preset: automasking crosses on the brush descriptor") {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    // Off by default, which is what keeps an existing host's stamps identical.
    CHECK(d.automask_factors == 0u);

    d.automask_factors = CLAY_AUTOMASK_BOUNDARY | CLAY_AUTOMASK_NORMAL_ANGLE;
    d.automask_boundary_rings = 3;
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.radius = 0.5f;

    clay_brush_preset p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_brush_preset_by_name("Standard", &p) == CLAY_OK);
    p.brush = d;

    size_t needed = 0;
    REQUIRE(clay_brush_preset_serialize(&p, nullptr, &needed) == CLAY_OK);
    std::vector<uint8_t> bytes(needed);
    size_t written = needed;
    REQUIRE(clay_brush_preset_serialize(&p, bytes.data(), &written) == CLAY_OK);

    clay_brush_preset back{};
    back.struct_size = sizeof(back);
    REQUIRE(clay_brush_preset_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
    CHECK(back.brush.automask_factors == d.automask_factors);
    CHECK(back.brush.automask_boundary_rings == 3);
}
