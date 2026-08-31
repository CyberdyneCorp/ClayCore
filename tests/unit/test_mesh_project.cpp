// Moving geometry onto a reference, and the guarantee that must NOT move with
// it (mesh-multires spec, add-mesh-multires).
//
// `transfer_attributes` promises it moves colours, UVs and normals and moves no
// position, and callers rely on that. Reprojection needs the same spatial query
// and the opposite contract, so the two are separate functions sharing a BVH —
// and the first case here is the one that says the old promise still holds.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/project.h"
#include "clay/mesh/transfer.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresError;
using mesh::MultiresSculptor;
using mesh::MultiresSurface;

namespace {

Mesh plane_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

MultiresSurface build(const Mesh& m, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(m, {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

// A bumpy sheet, as a dense triangle mesh — the "sculpt made somewhere else"
// that a clean cage has to be fitted to.
Mesh bumpy_reference(int n, float half, float amplitude) {
    Mesh m = plane_quads(n, half);
    for (cfloat3& p : m.positions)
        p.y = amplitude * std::sin(2.0f * p.x) * std::cos(2.0f * p.z);
    m.quads.clear();  // a reference is geometry, not a cage
    m.normals = mesh::vertex_normals(m);
    return m;
}

}  // namespace

TEST_CASE("attribute transfer still moves no position") {
    Mesh source = bumpy_reference(6, 2.0f, 0.4f);
    source.colors.assign(source.positions.size(), cf3(0.2f, 0.7f, 0.1f));
    Mesh target = plane_quads(4, 2.0f);
    const std::vector<cfloat3> before = target.positions;

    const mesh::TransferReport r = mesh::transfer_attributes(source, &target);
    CHECK(r.colors);
    REQUIRE(target.positions.size() == before.size());
    // BYTE-IDENTICAL. The guarantee this change was careful not to weaken.
    for (std::size_t v = 0; v < before.size(); ++v) {
        CHECK(target.positions[v].x == before[v].x);
        CHECK(target.positions[v].y == before[v].y);
        CHECK(target.positions[v].z == before[v].z);
    }
}

TEST_CASE("projection lands a flat sheet on the surface it is aimed at") {
    const Mesh reference = bumpy_reference(12, 2.0f, 0.4f);
    Mesh flat = plane_quads(8, 1.6f);
    std::vector<cfloat3> normals(flat.positions.size(), cf3(0, 1, 0));
    std::vector<cfloat3> positions = flat.positions;

    const mesh::ProjectReport r = mesh::project_surface(reference, normals, &positions);
    CHECK(r.moved == positions.size());
    CHECK(r.missed == 0);
    // The normal ray is what answered, not the closest-point fallback: on a
    // sheet the vertex is looking straight at its counterpart.
    CHECK(r.by_ray > r.by_closest);
    for (const cfloat3& p : positions) {
        const float expect = 0.4f * std::sin(2.0f * p.x) * std::cos(2.0f * p.z);
        CHECK(p.y == doctest::Approx(expect).epsilon(0.15));
    }
}

TEST_CASE("a distance limit leaves what it cannot reach exactly where it was") {
    const Mesh reference = bumpy_reference(12, 2.0f, 0.4f);
    std::vector<cfloat3> positions = {cf3(0.0f, 0.0f, 0.0f), cf3(0.0f, 40.0f, 0.0f)};
    const std::vector<cfloat3> before = positions;
    std::vector<cfloat3> normals(2, cf3(0, 1, 0));

    mesh::ProjectOptions options;
    options.max_distance = 1.0f;
    const mesh::ProjectReport r = mesh::project_surface(reference, normals, &positions, options);
    CHECK(r.missed == 1);
    CHECK(r.moved == 1);
    // Untouched rather than snapped to something arbitrary.
    CHECK(positions[1].y == before[1].y);
}

TEST_CASE("a sculpt projected onto a clean cage reproduces it within a stated tolerance") {
    // The route `set_base_mesh` names when it refuses: a hierarchy accepts a
    // sculpt made somewhere else by fitting every level to it, coarse first, so
    // each level projects from a parent that has already been fitted.
    const Mesh reference = bumpy_reference(16, 2.0f, 0.35f);
    MultiresSurface s = build(plane_quads(4, 2.0f), 3);

    mesh::ProjectReport report;
    mesh::ProjectOptions options;
    options.max_distance = 2.0f;
    REQUIRE(s.project_from(reference, options, &report));
    CHECK(report.moved > 0);

    // Every level now carries the detail the fit produced...
    CHECK_FALSE(s.detail_at(3).empty());
    // ...and the finest level agrees with the reference.
    const std::vector<cfloat3>& fine = s.positions_at(3);
    double worst = 0.0;
    for (const cfloat3& p : fine) {
        const float expect = 0.35f * std::sin(2.0f * p.x) * std::cos(2.0f * p.z);
        worst = std::max(worst, static_cast<double>(std::fabs(p.y - expect)));
    }
    CHECK(worst < 0.03);

    // And the fitted hierarchy still behaves like one: change the form and the
    // fitted detail comes with it.
    const std::uint64_t sum = s.detail_at(3).checksum();
    REQUIRE(s.set_sculpt_level(0));
    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 1.5f;
    settings.strength = 0.5f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);
    s.positions_at(3);
    CHECK(s.detail_at(3).checksum() == sum);
}

TEST_CASE("a cancelled projection reports what it did and refuses") {
    const Mesh reference = bumpy_reference(8, 2.0f, 0.3f);
    MultiresSurface s = build(plane_quads(4, 2.0f), 2);
    parallel::CancelToken token;
    token.cancel();
    mesh::ProjectReport report;
    CHECK_FALSE(s.project_from(reference, {}, &report, &token));
}
