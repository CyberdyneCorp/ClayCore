// The hierarchy: levels, evaluation, pricing, and the model the whole feature
// rests on (mesh-multires spec, add-mesh-multires).
//
//     P(n) = S(n) + Frame(n) * Detail(n)
//
// The assertions that matter are the two the spec spends: a level exports as an
// ORDINARY mesh that every existing consumer accepts unchanged, and a level's
// detail COEFFICIENTS are untouched by an edit below it — which is the numeric
// form of "the wrinkle is still there".

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "clay/mesh/multires.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/validate.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MultiresError;
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

Mesh cube_quads() {
    Mesh m;
    m.positions = {cf3(-1, -1, -1), cf3(1, -1, -1), cf3(1, -1, 1), cf3(-1, -1, 1),
                   cf3(-1, 1, -1),  cf3(1, 1, -1),  cf3(1, 1, 1),  cf3(-1, 1, 1)};
    const std::uint32_t faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                       {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    for (const auto& f : faces) {
        m.quads.insert(m.quads.end(), {f[0], f[1], f[2], f[3]});
        m.indices.insert(m.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
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

}  // namespace

TEST_CASE("a cage with nothing above it is a hierarchy of one level") {
    MultiresSurface s = build(cube_quads(), 0);
    CHECK(s.valid());
    CHECK(s.level_count() == 1);
    CHECK(s.max_level() == 0);
    CHECK(s.sculpt_level() == 0);
    CHECK(s.display_level() == 0);
    CHECK(s.base_mesh().positions.size() == 8);
    CHECK(s.detail_at(0).vertex_count() == 0);  // the cage stores no displacement
}

TEST_CASE("a cage that cannot carry a hierarchy is refused with a reason") {
    MultiresError err = MultiresError::None;

    CHECK_FALSE(MultiresSurface::from_mesh(Mesh{}, {}, &err).has_value());
    CHECK(err == MultiresError::EmptyBase);

    Mesh bad_index;
    bad_index.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 0, 1)};
    bad_index.indices = {0, 1, 99};
    CHECK_FALSE(MultiresSurface::from_mesh(bad_index, {}, &err).has_value());
    CHECK(err == MultiresError::IndexOutOfRange);

    // A quad whose corners weld together is not a quad, and every level above
    // it would carry the same collapse.
    Mesh degenerate;
    degenerate.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(1, 0, 1), cf3(0, 0, 0)};
    degenerate.quads = {0, 1, 2, 3};
    degenerate.indices = {0, 1, 2, 0, 2, 3};
    CHECK_FALSE(MultiresSurface::from_mesh(degenerate, {}, &err).has_value());
    CHECK(err == MultiresError::DegenerateFace);

    // Three faces on one edge: the subdivision rules ask "which two faces" and
    // there is no answer.
    Mesh fin;
    fin.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1), cf3(0, -1, 0)};
    fin.indices = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    CHECK_FALSE(MultiresSurface::from_mesh(fin, {}, &err).has_value());
    CHECK(err == MultiresError::NonManifold);
}

TEST_CASE("a level exports as an ordinary mesh every existing consumer accepts") {
    MultiresSurface s = build(cube_quads(), 3);
    CHECK(s.level_count() == 4);

    for (std::uint32_t l = 0; l <= 3; ++l) {
        const Mesh m = s.mesh_at_level(l);
        CHECK(!m.positions.empty());
        CHECK(!m.indices.empty());
        // mesh_data.h's invariant: when quads are present, `indices` is exactly
        // their triangulation over the same positions. Every consumer in this
        // library relies on it and nothing here may be the first to break it.
        CHECK(mesh::quads_consistent(m));
        const mesh::ValidationReport r = mesh::validate(m);
        CHECK(r.watertight);
        CHECK(r.manifold);
        CHECK(r.oriented);
        CHECK(r.degenerate_triangles == 0);
        CHECK(r.euler_characteristic == 2);
        for (const cfloat3& p : m.positions) {
            CHECK(std::isfinite(p.x));
            CHECK(std::isfinite(p.y));
            CHECK(std::isfinite(p.z));
        }
    }
}

TEST_CASE("level zero exports the cage it was given") {
    const Mesh base = cube_quads();
    MultiresSurface s = build(base, 2);
    const Mesh out = s.mesh_at_level(0);
    REQUIRE(out.positions.size() == base.positions.size());
    for (std::size_t v = 0; v < base.positions.size(); ++v) {
        CHECK(out.positions[v].x == base.positions[v].x);
        CHECK(out.positions[v].y == base.positions[v].y);
        CHECK(out.positions[v].z == base.positions[v].z);
    }
    CHECK(out.quads == base.quads);
    CHECK(out.indices == base.indices);
}

TEST_CASE("a level is its subdivided parent plus its own detail, exactly") {
    MultiresSurface s = build(plane_quads(2, 1.0f), 2);
    const std::uint32_t n = s.topology_at(2).vertex_count;
    REQUIRE(n > 0);

    // With no detail the level IS the pure subdivision, byte for byte.
    {
        const std::vector<cfloat3>& p = s.positions_at(2);
        const std::vector<cfloat3>& sub = s.subdivided_at(2);
        REQUIRE(p.size() == sub.size());
        for (std::size_t v = 0; v < p.size(); ++v) {
            CHECK(p[v].x == sub[v].x);
            CHECK(p[v].y == sub[v].y);
            CHECK(p[v].z == sub[v].z);
        }
    }

    const std::uint32_t target = n / 2;
    s.set_detail(2, target, LocalDetail{0.0f, 0.0f, 0.25f});
    const std::vector<mesh::SurfaceFrame>& frames = s.frames_at(2);
    const cfloat3 sub = s.subdivided_at(2)[target];
    const cfloat3 expected =
        sub + mesh::frame_to_world(frames[target], 0.0f, 0.0f, 0.25f);
    const cfloat3 got = s.positions_at(2)[target];
    CHECK(got.x == doctest::Approx(expected.x));
    CHECK(got.y == doctest::Approx(expected.y));
    CHECK(got.z == doctest::Approx(expected.z));
    // A flat sheet: a normal-only coefficient lifts straight off it.
    CHECK(std::fabs(got.y) == doctest::Approx(0.25f).epsilon(1e-4));
}

TEST_CASE("THE SIGNATURE PROPERTY: detail survives an edit to the form beneath it") {
    // Sculpt fine detail, change the coarse form, and come back. The
    // COEFFICIENTS must be untouched — that is what "the wrinkle is still
    // there" means numerically — and the reconstructed offset must still be
    // normal to the surface, which is what "and it moved with the nose" means.
    MultiresSurface s = build(plane_quads(4, 2.0f), 3);
    const std::uint32_t fine = 3;
    const std::uint32_t n = s.topology_at(fine).vertex_count;

    std::vector<std::uint32_t> detailed;
    for (std::uint32_t v = 0; v < n; v += 11) {
        s.set_detail(fine, v, LocalDetail{0.0f, 0.0f, 0.04f});
        detailed.push_back(v);
    }
    const std::uint64_t before = s.detail_at(fine).checksum();
    const std::vector<cfloat3> fine_before = s.positions_at(fine);

    // Now change the FORM: lift the middle of the cage, which is an edit two
    // levels below the detail.
    const std::uint32_t base_n = s.topology_at(0).vertex_count;
    for (std::uint32_t v = 0; v < base_n; ++v) {
        const cfloat3 p = s.positions_at(0)[v];
        if (std::fabs(p.x) < 1.5f && std::fabs(p.z) < 1.5f)
            s.set_base_position(v, cf3(p.x, p.y + 0.8f, p.z));
    }

    const std::vector<cfloat3> fine_after = s.positions_at(fine);
    // 1. The form moved.
    float max_move = 0.0f;
    for (std::size_t v = 0; v < fine_after.size(); ++v)
        max_move = std::max(max_move, std::fabs(fine_after[v].y - fine_before[v].y));
    CHECK(max_move > 0.3f);

    // 2. The detail is untouched, coefficient for coefficient.
    CHECK(s.detail_at(fine).checksum() == before);
    for (std::uint32_t v : detailed) {
        const LocalDetail d = s.detail_at(fine).get(v);
        CHECK(d.normal == doctest::Approx(0.04f));
        CHECK(d.tangent == doctest::Approx(0.0f));
    }

    // 3. And it is still ATTACHED: the reconstructed offset from the pure
    //    subdivision is the same length and still along the surface normal.
    const std::vector<cfloat3>& sub = s.subdivided_at(fine);
    const std::vector<mesh::SurfaceFrame>& frames = s.frames_at(fine);
    for (std::uint32_t v : detailed) {
        const cfloat3 offset = fine_after[v] - sub[v];
        CHECK(clength(offset) == doctest::Approx(0.04f).epsilon(1e-3));
        CHECK(cdot(offset, frames[v].normal) == doctest::Approx(0.04f).epsilon(1e-3));
    }
}

TEST_CASE("adding a level reports its cost before paying it, and refuses over budget") {
    Mesh base = plane_quads(8, 4.0f);
    mesh::MultiresOptions options;
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(base, options, &err);
    REQUIRE(surface.has_value());

    const mesh::MultiresPreflight p = surface->preflight_add_level();
    CHECK(p.level == 1);
    CHECK(p.allowed);
    // Level 1 over an n-by-n quad grid: V + E + F new vertices, and one child
    // face per parent corner.
    CHECK(p.faces == 8u * 8u * 4u);
    CHECK(p.vertices > 0);
    CHECK(p.topology_bytes > 0);
    CHECK(p.peak_bytes >= p.persistent_bytes);
    CHECK(surface->preflight_add_level().level == 1);  // no side effects

    // The same surface with a budget below that cost refuses, and is unchanged.
    options.memory_budget = p.peak_bytes / 2;
    auto tight = MultiresSurface::from_mesh(base, options, &err);
    REQUIRE(tight.has_value());
    const mesh::MultiresPreflight refused = tight->preflight_add_level();
    CHECK_FALSE(refused.allowed);
    CHECK(refused.error == MultiresError::OverBudget);
    CHECK_FALSE(tight->add_level(&err));
    CHECK(err == MultiresError::OverBudget);
    CHECK(tight->level_count() == 1);
    CHECK(tight->memory().topology == surface->memory().topology);
}

TEST_CASE("a cancelled add_level leaves the surface exactly as it was") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 1);
    const std::uint64_t before = s.detail_checksum();
    const std::uint32_t levels = s.level_count();

    parallel::CancelToken token;
    token.cancel();
    MultiresError err = MultiresError::None;
    CHECK_FALSE(s.add_level(&err, &token));
    CHECK(err == MultiresError::Cancelled);
    CHECK(s.level_count() == levels);
    CHECK(s.detail_checksum() == before);
}

TEST_CASE("removing the highest level hands back what it destroyed") {
    MultiresSurface s = build(plane_quads(2, 1.0f), 2);
    s.set_detail(2, 4, LocalDetail{0.0f, 0.0f, 0.3f});
    const std::uint64_t sum = s.detail_at(2).checksum();

    mesh::DetailField rescued;
    MultiresError err = MultiresError::None;
    REQUIRE(s.remove_highest_level(&err, &rescued));
    CHECK(s.level_count() == 2);
    CHECK(s.sculpt_level() <= 1);
    CHECK(s.display_level() <= 1);
    // What it destroyed is handed out rather than dropped on the floor, which
    // is what makes the operation reversible by the owner above it.
    CHECK(rescued.checksum() == sum);

    CHECK(s.remove_highest_level(&err));
    CHECK(s.level_count() == 1);
    CHECK_FALSE(s.remove_highest_level(&err));
    CHECK(err == MultiresError::NoLevelToRemove);
}

TEST_CASE("a hierarchy carrying detail refuses a new cage and names the route that works") {
    MultiresSurface s = build(plane_quads(2, 1.0f), 2);
    MultiresError err = MultiresError::None;

    // With no detail the cage can still be replaced.
    CHECK(s.set_base_mesh(plane_quads(2, 1.5f), &err));
    CHECK(err == MultiresError::None);
    CHECK(s.level_count() == 3);

    s.set_detail(2, 4, LocalDetail{0.0f, 0.0f, 0.3f});
    CHECK_FALSE(s.set_base_mesh(plane_quads(2, 2.0f), &err));
    CHECK(err == MultiresError::DetailPresent);
    CHECK(std::string(mesh::multires_error_text(err)).find("project") != std::string::npos);
}

TEST_CASE("dropping the caches costs nothing authoritative") {
    MultiresSurface s = build(cube_quads(), 3);
    s.set_detail(3, 100, LocalDetail{0.01f, -0.02f, 0.05f});
    s.set_detail(2, 30, LocalDetail{0.0f, 0.0f, 0.1f});

    const std::vector<cfloat3> before = s.positions_at(3);
    const std::uint64_t sum = s.detail_checksum();
    const mesh::MultiresMemory loaded = s.memory();
    CHECK(loaded.rebuildable > 0);
    CHECK(loaded.authoritative > 0);
    CHECK(loaded.total == loaded.authoritative + loaded.rebuildable);

    s.drop_all_caches();
    CHECK_FALSE(s.level_resident(3));
    const mesh::MultiresMemory bare = s.memory();
    CHECK(bare.rebuildable == 0);
    // Detail is NEVER reported as rebuildable: a host acting on that
    // distinction would delete the user's work.
    CHECK(bare.authoritative == loaded.authoritative);
    CHECK(s.detail_checksum() == sum);

    const std::vector<cfloat3>& after = s.positions_at(3);
    REQUIRE(after.size() == before.size());
    for (std::size_t v = 0; v < after.size(); ++v) {
        CHECK(after[v].x == before[v].x);
        CHECK(after[v].y == before[v].y);
        CHECK(after[v].z == before[v].z);
    }
}

TEST_CASE("the sculpt level and the display level are independent") {
    MultiresSurface s = build(cube_quads(), 3);
    CHECK(s.set_sculpt_level(1));
    CHECK(s.set_display_level(3));
    CHECK(s.sculpt_level() == 1);
    CHECK(s.display_level() == 3);
    CHECK_FALSE(s.set_sculpt_level(9));
    CHECK(s.sculpt_level() == 1);
}

TEST_CASE("a UV seam is interpolated along itself and never across itself") {
    // Two quads sharing an edge geometrically, but split in the index buffer
    // the way a flat mesh writes a seam: the shared positions are duplicated
    // and carry different UVs. Averaging across the seam would pull the two
    // sides toward each other and destroy it.
    Mesh m;
    m.positions = {cf3(-1, 0, -1), cf3(0, 0, -1), cf3(0, 0, 1), cf3(-1, 0, 1),
                   cf3(0, 0, -1),  cf3(1, 0, -1), cf3(1, 0, 1), cf3(0, 0, 1)};
    m.uvs = {cf2(0, 0), cf2(0.5f, 0), cf2(0.5f, 1), cf2(0, 1),
             cf2(1, 0), cf2(0.5f, 0), cf2(0.5f, 1), cf2(1, 1)};
    m.quads = {0, 1, 2, 3, 4, 5, 6, 7};
    m.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    MultiresSurface s = build(m, 2);

    const Mesh out = s.mesh_at_level(2);
    REQUIRE(out.uvs.size() == out.positions.size());
    // The two sides of the seam still hold their own u: 0.5 on one side and 0.5
    // on the other reached from 1.0, never a blend of the two neighbourhoods.
    float min_u = 1e9f, max_u = -1e9f;
    for (const cfloat2& uv : out.uvs) {
        min_u = std::min(min_u, uv.x);
        max_u = std::max(max_u, uv.x);
    }
    CHECK(min_u == doctest::Approx(0.0f));
    CHECK(max_u == doctest::Approx(1.0f));

    // And the GEOMETRY is welded across the seam: the two sides are one surface
    // and the subdivision must not open a crack down the middle of it.
    const std::vector<cfloat3>& geom = s.positions_at(2);
    CHECK(geom.size() < out.positions.size());
    for (const cfloat3& p : geom) CHECK(std::fabs(p.y) == doctest::Approx(0.0f));
}
