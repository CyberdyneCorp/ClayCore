// The Layer ceiling across a depth boundary, driven through the C ABI a host
// ships: `clay_multires_sculptor_stamp` with CLAY_MESH_BRUSH_LAYER, and a
// `clay_multires_trim` landing between two dabs of one gesture, which is what an
// operating-system memory warning does to a live stroke.
//
// The C ABI has no regional refinement of its own, so the hierarchy is built in
// C++ and handed over as the bytes `clay_multires_deserialize` reads -- the same
// route a hierarchy authored in pyclay and saved takes into a host.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/mesh/multires.h"

namespace {

// Every C call below is expected to succeed; one line per call keeps the
// helpers readable rather than one REQUIRE per argument list.
void must(clay_result r) { REQUIRE(r == CLAY_OK); }

// The 6x6 cage `test_multires_sculpt.cpp`'s regional gates use.
clay::mesh::Mesh bumpy_quads() {
    using clay::kernel::cf3;
    clay::mesh::Mesh cage;
    constexpr std::uint32_t n = 6;
    constexpr float step = 2.0f / static_cast<float>(n);
    for (std::uint32_t z = 0; z <= n; ++z)
        for (std::uint32_t x = 0; x <= n; ++x)
            cage.positions.push_back(cf3(-1.0f + step * static_cast<float>(x),
                                         0.15f * static_cast<float>((x * 7 + z * 3) % 5),
                                         -1.0f + step * static_cast<float>(z)));
    constexpr std::uint32_t stride = n + 1;
    for (std::uint32_t z = 0; z < n; ++z)
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::uint32_t a = z * stride + x, b = a + 1, c = a + stride + 1, d = a + stride;
            cage.quads.insert(cage.quads.end(), {a, b, c, d});
            cage.indices.insert(cage.indices.end(), {a, b, c, a, c, d});
        }
    return cage;
}

// That cage refined over its middle 2x2 patches to level 3, as bytes.
std::vector<std::uint8_t> regional_bytes() {
    clay::mesh::MultiresError err = clay::mesh::MultiresError::None;
    auto s = clay::mesh::MultiresSurface::from_mesh(bumpy_quads(), {}, &err);
    REQUIRE(s.has_value());
    const std::vector<std::uint32_t> block = {14, 15, 20, 21};  // z * 6 + x, z and x in {2, 3}
    REQUIRE(s->refine_patches_to_level(block, 3));
    return s->encode();
}

std::vector<float> level_positions(clay_multires* surface, uint32_t level) {
    clay_mesh* m = nullptr;
    must(clay_multires_copy_level_mesh(surface, level, &m));
    const float* p = clay_mesh_positions(m);
    std::vector<float> out(p, p + clay_mesh_vertex_count(m) * 3);
    clay_mesh_destroy(m);
    return out;
}

float worst_travel(const std::vector<float>& was, const std::vector<float>& now) {
    REQUIRE(was.size() == now.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < was.size(); i += 3) {
        const float dx = now[i] - was[i], dy = now[i + 1] - was[i + 1], dz = now[i + 2] - was[i + 2];
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return worst;
}

// The level-3 vertex nearest the rim of the refined region.
void rim_center(clay_multires* surface, float out[3]) {
    const std::vector<float> p = level_positions(surface, 3);
    float best = 1e30f;
    for (std::size_t i = 0; i < p.size(); i += 3) {
        const float dx = p[i] + 1.0f / 3.0f, dz = p[i + 2];
        const float d = dx * dx + p[i + 1] * p[i + 1] + dz * dz;
        if (d < best) {
            best = d;
            std::memcpy(out, &p[i], sizeof(float) * 3);
        }
    }
}

// A regional hierarchy a host opened from bytes, at its sculpt level.
clay_multires* open_regional(const std::vector<std::uint8_t>& bytes) {
    clay_multires* surface = nullptr;
    must(clay_multires_deserialize(bytes.data(), bytes.size(), &surface));
    must(clay_multires_set_sculpt_level(surface, 3));
    int32_t uniform = 1;
    must(clay_multires_uniform_depth(surface, &uniform));
    REQUIRE(uniform == 0);  // regional, or there is no coarse side to cross onto
    return surface;
}

clay_mesh_brush_desc rim_layer_brush(clay_multires* surface) {
    clay_mesh_brush_desc brush{};
    brush.struct_size = sizeof(brush);
    must(clay_mesh_brush_defaults(&brush));
    brush.verb = CLAY_MESH_BRUSH_LAYER;
    rim_center(surface, brush.center);
    brush.radius = 0.50f;  // anchored on the rim and reaching well past it
    brush.strength = 1.0f;
    brush.layer_height = 0.08f;
    return brush;
}

void stamp(clay_multires_sculptor* sculptor, const clay_mesh_brush_desc& brush) {
    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    must(clay_multires_sculptor_stamp(sculptor, &brush, nullptr, &report));
    REQUIRE(report.moved_vertices > 0);
}

uint64_t seed_token(clay_multires_sculptor* sculptor) {
    uint64_t token = 0;
    must(clay_multires_sculptor_seed_revision(sculptor, &token));
    return token;
}

struct Run {
    std::vector<float> coarse;
    bool rebound = false;
};

// Two LAYER dabs in one gesture, with a trim at `pressure` between them when
// `trim` is set.
Run layer_stroke(const std::vector<std::uint8_t>& bytes, bool trim, int32_t pressure) {
    clay_multires* surface = open_regional(bytes);
    const clay_mesh_brush_desc brush = rim_layer_brush(surface);
    clay_multires_sculptor* sculptor = nullptr;
    must(clay_multires_sculptor_create(surface, &sculptor));
    must(clay_multires_sculptor_begin_stroke(sculptor));
    stamp(sculptor, brush);

    const uint64_t token = seed_token(sculptor);
    if (trim) {
        clay_trim_report trimmed{};
        trimmed.struct_size = sizeof(trimmed);
        must(clay_multires_trim(surface, pressure, nullptr, &trimmed));
    }
    stamp(sculptor, brush);

    Run out;
    // Read after the second dab: the token is minted when the level sculptor is
    // rebuilt, so a changed one is the rebind itself and not the probe's.
    out.rebound = seed_token(sculptor) != token;
    out.coarse = level_positions(surface, 2);
    clay_multires_sculptor_destroy(sculptor);
    clay_multires_destroy(surface);
    return out;
}

}  // namespace

TEST_CASE("c regression: a trim mid-stroke does not lift the Layer ceiling at a depth boundary") {
    // A generation-only rebind used to empty each coarse level's stroke record
    // while the bound level's survived, so the coarse side deposited its whole
    // ceiling again after the trim: a step at the seam, only when the host was
    // short of memory. The host's own negative repro never trimmed mid-stroke,
    // so it could not see this; here the rebind is asserted, not assumed.
    const std::vector<std::uint8_t> bytes = regional_bytes();

    clay_multires* fresh = nullptr;
    must(clay_multires_deserialize(bytes.data(), bytes.size(), &fresh));
    const std::vector<float> pristine = level_positions(fresh, 2);
    clay_multires_destroy(fresh);

    const Run kept = layer_stroke(bytes, false, CLAY_PRESSURE_NONE);
    REQUIRE_FALSE(kept.rebound);
    const float settled = worst_travel(pristine, kept.coarse);
    CHECK(settled > 0.01f);  // the stroke reached the coarse side at all
    CHECK(settled <= 0.08f);

    for (int32_t pressure : {CLAY_PRESSURE_URGENT, CLAY_PRESSURE_CRITICAL}) {
        CAPTURE(pressure);
        const Run trimmed = layer_stroke(bytes, true, pressure);
        REQUIRE(trimmed.rebound);
        const float travel = worst_travel(pristine, trimmed.coarse);
        MESSAGE("coarse travel " << travel << " after trim(" << pressure
                                 << ") mid-stroke, ceiling 0.08");
        CHECK(travel <= 0.08f);
        CHECK(trimmed.coarse == kept.coarse);
    }
}
