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

// The 6x6 cage `test_multires_sculpt.cpp`'s regional gates use, refined over
// its middle 2x2 patches to level 3.
std::vector<std::uint8_t> regional_bytes() {
    using clay::kernel::cf3;
    clay::mesh::Mesh cage;
    const int n = 6;
    const float half = 1.0f, step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            cage.positions.push_back(cf3(-half + step * static_cast<float>(x),
                                         0.15f * static_cast<float>((x * 7 + z * 3) % 5),
                                         -half + step * static_cast<float>(z)));
    const std::uint32_t stride = n + 1;
    for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(n); ++z)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(n); ++x) {
            const std::uint32_t a = z * stride + x, b = a + 1, c = a + stride + 1, d = a + stride;
            cage.quads.insert(cage.quads.end(), {a, b, c, d});
            cage.indices.insert(cage.indices.end(), {a, b, c, a, c, d});
        }
    clay::mesh::MultiresError err = clay::mesh::MultiresError::None;
    auto s = clay::mesh::MultiresSurface::from_mesh(cage, {}, &err);
    REQUIRE(s.has_value());
    std::vector<std::uint32_t> block;
    for (std::uint32_t z = 2; z < 4; ++z)
        for (std::uint32_t x = 2; x < 4; ++x) block.push_back(z * 6 + x);
    REQUIRE(s->refine_patches_to_level(block, 3));
    return s->encode();
}

std::vector<float> level_positions(clay_multires* surface, uint32_t level) {
    clay_mesh* m = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(surface, level, &m) == CLAY_OK);
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

struct Run {
    std::vector<float> coarse;
    bool rebound = false;
};

// Two LAYER dabs in one gesture, with a trim at `pressure` between them when
// `trim` is set.
Run layer_stroke(const std::vector<std::uint8_t>& bytes, bool trim, int32_t pressure) {
    clay_multires* surface = nullptr;
    REQUIRE(clay_multires_deserialize(bytes.data(), bytes.size(), &surface) == CLAY_OK);
    REQUIRE(clay_multires_set_sculpt_level(surface, 3) == CLAY_OK);
    int32_t uniform = 1;
    REQUIRE(clay_multires_uniform_depth(surface, &uniform) == CLAY_OK);
    REQUIRE(uniform == 0);  // regional, or there is no coarse side to cross onto

    clay_mesh_brush_desc brush{};
    brush.struct_size = sizeof(brush);
    REQUIRE(clay_mesh_brush_defaults(&brush) == CLAY_OK);
    brush.verb = CLAY_MESH_BRUSH_LAYER;
    rim_center(surface, brush.center);
    brush.radius = 0.50f;  // anchored on the rim and reaching well past it
    brush.strength = 1.0f;
    brush.layer_height = 0.08f;

    clay_multires_sculptor* sculptor = nullptr;
    REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);
    REQUIRE(clay_multires_sculptor_begin_stroke(sculptor) == CLAY_OK);
    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_multires_sculptor_stamp(sculptor, &brush, nullptr, &report) == CLAY_OK);
    REQUIRE(report.moved_vertices > 0);

    uint64_t token = 0;
    REQUIRE(clay_multires_sculptor_seed_revision(sculptor, &token) == CLAY_OK);
    if (trim) {
        clay_trim_report trimmed{};
        trimmed.struct_size = sizeof(trimmed);
        REQUIRE(clay_multires_trim(surface, pressure, nullptr, &trimmed) == CLAY_OK);
    }
    REQUIRE(clay_multires_sculptor_stamp(sculptor, &brush, nullptr, &report) == CLAY_OK);
    REQUIRE(report.moved_vertices > 0);

    Run out;
    // Read after the second dab: the token is minted when the level sculptor is
    // rebuilt, so a changed one is the rebind itself and not the probe's.
    uint64_t after = 0;
    REQUIRE(clay_multires_sculptor_seed_revision(sculptor, &after) == CLAY_OK);
    out.rebound = after != token;
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
    REQUIRE(clay_multires_deserialize(bytes.data(), bytes.size(), &fresh) == CLAY_OK);
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
