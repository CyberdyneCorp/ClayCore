// Split, collapse and flip: atomicity, refusals, and the fuzz run
// (dynamic-topology spec, add-dynamic-topology).
//
// THE FUZZ CASE AT THE BOTTOM IS THE POINT OF THIS FILE. The other tests pin
// behaviours somebody thought of; the fuzz run is what catches the ones nobody
// did. A half-edge operator's failure mode is a surface that still renders,
// still exports, still takes another stroke and is quietly wrong in one fan, and
// thousands of interleaved operations with the validator run after every one is
// the only way that surfaces at all.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/topology_ops.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSurface;
using mesh::EdgeConstraint;
using mesh::EdgeId;
using mesh::Mesh;
using mesh::TopologyResult;

namespace {

Mesh plane_grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

Mesh cube_sphere(int n, float radius) {
    Mesh m;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const cfloat3 p = cf3(c[0], c[1], c[2]);
                const cfloat3 unit = p / clength(p);
                m.positions.push_back(unit * radius);
                m.normals.push_back(unit);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

// A deterministic generator. NOT std::mt19937 seeded from a device: the fuzz
// run has to reproduce exactly when it fails, or a rare failure is a failure
// nobody can chase.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n) { return n ? next() % n : 0; }
    float unit() { return static_cast<float>(next() % 100000u) / 100000.0f; }
};

// The interior edges, in SLOT ORDER. The determinism rule: a candidate set is
// ordered by stable id before any operator runs, so the same input produces the
// same sequence on every platform.
std::vector<EdgeId> live_edges(const DynamicSurface& s) {
    std::vector<EdgeId> out;
    s.edges().for_each_live([&](EdgeId id, const mesh::DynamicEdge&) { out.push_back(id); });
    return out;
}

}  // namespace

TEST_CASE("topology ops: an interior split makes two faces into four") {
    Mesh grid = plane_grid(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    const mesh::DynamicSurfaceStats before = surface->stats();

    // An interior edge: one with a face on both sides.
    EdgeId interior;
    for (EdgeId e : live_edges(*surface))
        if (!surface->is_boundary_edge(e)) {
            interior = e;
            break;
        }
    REQUIRE(interior.valid());

    const mesh::SplitResult r = mesh::split_edge(*surface, interior, 0.5f);
    CHECK(r.result == TopologyResult::Ok);
    CHECK(r.vertex.valid());
    CHECK(r.face_count == 4);

    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    REQUIRE(report.ok);

    const mesh::DynamicSurfaceStats after = surface->stats();
    CHECK(after.vertices == before.vertices + 1);
    CHECK(after.faces == before.faces + 2);   // two faces became four
    CHECK(after.edges == before.edges + 3);   // the split edge, plus two spokes
    CHECK(after.halfedges == after.edges * 2);
}

TEST_CASE("topology ops: a boundary split makes one face into two") {
    Mesh grid = plane_grid(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    const mesh::DynamicSurfaceStats before = surface->stats();

    EdgeId border;
    for (EdgeId e : live_edges(*surface))
        if (surface->is_boundary_edge(e)) {
            border = e;
            break;
        }
    REQUIRE(border.valid());

    const mesh::SplitResult r = mesh::split_edge(*surface, border, 0.5f);
    CHECK(r.result == TopologyResult::Ok);
    CHECK(r.face_count == 2);

    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    REQUIRE(report.ok);

    const mesh::DynamicSurfaceStats after = surface->stats();
    CHECK(after.vertices == before.vertices + 1);
    CHECK(after.faces == before.faces + 1);
    // The boundary got one edge longer, not shorter: a split never closes a
    // border.
    CHECK(after.boundary_edges == before.boundary_edges + 1);
}

TEST_CASE("topology ops: a split interpolates the attributes it carries") {
    Mesh grid = plane_grid(2, 1.0f);
    grid.colors.assign(grid.positions.size(), cf3(0, 0, 0));
    // Two ends of one edge, coloured differently, so the midpoint is checkable.
    grid.colors[0] = cf3(1, 0, 0);
    grid.colors[1] = cf3(0, 0, 1);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());

    // The edge between vertices 0 and 1.
    EdgeId target;
    for (EdgeId e : live_edges(*surface)) {
        const mesh::HalfEdgeId h = surface->halfedge_of(e);
        const std::uint32_t a = surface->origin_of(h).slot;
        const std::uint32_t b = surface->target_of(h).slot;
        if ((a == 0 && b == 1) || (a == 1 && b == 0)) {
            target = e;
            break;
        }
    }
    REQUIRE(target.valid());

    // WHICH WAY the edge runs is the surface's business, not the caller's: an
    // edge is a pair of half-edges and either may be the one the edge names. So
    // the expectation is read from the actual direction rather than assumed —
    // the first draft of this test assumed and was measuring the split's
    // arithmetic against its own guess.
    const mesh::HalfEdgeId h = surface->halfedge_of(target);
    const bool forward = surface->origin_of(h).slot == 0;
    const mesh::SplitResult r = mesh::split_edge(*surface, target, 0.25f);
    REQUIRE(r.result == TopologyResult::Ok);
    const mesh::DynamicVertex* mid = surface->vertex(r.vertex);
    REQUIRE(mid != nullptr);
    // A quarter of the way along, from whichever end the edge starts at.
    CHECK(mid->color.x == doctest::Approx(forward ? 0.75f : 0.25f));
    CHECK(mid->color.z == doctest::Approx(forward ? 0.25f : 0.75f));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology ops: a locked edge refuses to split, and changes nothing") {
    Mesh grid = plane_grid(3, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    const mesh::DynamicSurfaceStats before = surface->stats();

    const EdgeId e = live_edges(*surface).front();
    surface->edges_mutable().at(e).constraints |= EdgeConstraint::UserLocked;

    const mesh::SplitResult r = mesh::split_edge(*surface, e);
    CHECK(r.result == TopologyResult::Constrained);
    CHECK_FALSE(r.vertex.valid());

    // ATOMIC: a refused operation changes NOTHING. Not "mostly nothing".
    const mesh::DynamicSurfaceStats after = surface->stats();
    CHECK(after.vertices == before.vertices);
    CHECK(after.edges == before.edges);
    CHECK(after.faces == before.faces);
    CHECK(after.halfedges == before.halfedges);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology ops: an interior collapse removes a vertex and two faces") {
    Mesh sphere = cube_sphere(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(sphere);
    REQUIRE(surface.has_value());
    const mesh::DynamicSurfaceStats before = surface->stats();

    std::size_t collapsed = 0;
    for (EdgeId e : live_edges(*surface)) {
        const mesh::CollapseResult r = mesh::collapse_edge(*surface, e);
        if (r.result != TopologyResult::Ok) continue;
        ++collapsed;
        const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
        CAPTURE(report.summary());
        REQUIRE(report.ok);
        break;
    }
    REQUIRE(collapsed == 1);

    const mesh::DynamicSurfaceStats after = surface->stats();
    CHECK(after.vertices == before.vertices - 1);
    CHECK(after.faces == before.faces - 2);
    CHECK(after.edges == before.edges - 3);
    CHECK(after.halfedges == after.edges * 2);
    // Still a sphere: the Euler characteristic is the strongest single
    // statement that a collapse did not change the surface's topology.
    const long euler = static_cast<long>(after.vertices) - static_cast<long>(after.edges) +
                       static_cast<long>(after.faces);
    CHECK(euler == 2);
}

TEST_CASE("topology ops: a collapse that would pinch the surface is refused") {
    // THE LINK CONDITION. Two vertices sharing a neighbour that is not opposite
    // the edge cannot be merged without pinching the surface into a
    // non-manifold state — which renders fine and is unusable afterwards.
    //
    // A tetrahedron is the smallest surface where every collapse fails it: each
    // edge's endpoints share BOTH other vertices, and only two of them are
    // opposite.
    Mesh tet;
    tet.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1)};
    tet.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    auto surface = DynamicSurface::from_mesh(tet);
    REQUIRE(surface.has_value());
    REQUIRE(mesh::validate_dynamic_surface(*surface).ok);
    const mesh::DynamicSurfaceStats before = surface->stats();

    std::size_t refused = 0;
    for (EdgeId e : live_edges(*surface)) {
        const mesh::CollapseResult r = mesh::collapse_edge(*surface, e);
        CHECK(r.result != TopologyResult::Ok);
        ++refused;
    }
    CHECK(refused == 6);

    // Every one of them left the surface exactly as it was.
    const mesh::DynamicSurfaceStats after = surface->stats();
    CHECK(after.vertices == before.vertices);
    CHECK(after.faces == before.faces);
    CHECK(after.edges == before.edges);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology ops: a constrained edge refuses to collapse") {
    Mesh sphere = cube_sphere(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(sphere);
    REQUIRE(surface.has_value());

    const EdgeId e = live_edges(*surface).front();
    surface->edges_mutable().at(e).constraints |= EdgeConstraint::Sharp;
    CHECK(mesh::collapse_edge(*surface, e).result == TopologyResult::Constrained);

    surface->edges_mutable().at(e).constraints = 0;
    surface->edges_mutable().at(e).constraints |= EdgeConstraint::UserLocked;
    CHECK(mesh::collapse_edge(*surface, e).result == TopologyResult::Constrained);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology ops: a flip improves the pair or declines") {
    // Two triangles forming a long thin quad: the diagonal along the short axis
    // gives two slivers, and flipping to the other one is a clear improvement.
    Mesh m;
    m.positions = {cf3(-2, 0, 0), cf3(2, 0, 0), cf3(0, 0, -0.25f), cf3(0, 0, 0.25f)};
    m.normals.assign(4, cf3(0, 1, 0));
    m.indices = {0, 3, 2, 1, 2, 3};
    auto surface = DynamicSurface::from_mesh(m);
    REQUIRE(surface.has_value());
    REQUIRE(mesh::validate_dynamic_surface(*surface).ok);

    EdgeId interior;
    for (EdgeId e : live_edges(*surface))
        if (!surface->is_boundary_edge(e)) interior = e;
    REQUIRE(interior.valid());

    // The pair has a valence-3 vertex on each end of the shared edge, which the
    // operator refuses on its own — flipping would leave a two-edge fan.
    const mesh::FlipResult r = mesh::flip_edge(*surface, interior);
    CHECK(r.result == TopologyResult::LinkCondition);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology ops: a flip on a grid rewires exactly two faces") {
    Mesh grid = plane_grid(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    const mesh::DynamicSurfaceStats before = surface->stats();

    std::size_t flipped = 0;
    for (EdgeId e : live_edges(*surface)) {
        if (surface->is_boundary_edge(e)) continue;
        // `force`, because a regular grid's diagonals are already as good as
        // the flip would make them — the quality gate is doing its job, and
        // this case is about the REWIRE rather than about the policy.
        const mesh::FlipResult r = mesh::flip_edge(*surface, e, {}, nullptr, /*force=*/true);
        if (r.result != TopologyResult::Ok) continue;
        ++flipped;
        const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
        CAPTURE(report.summary());
        REQUIRE(report.ok);
        break;
    }
    REQUIRE(flipped == 1);

    // A flip changes NO counts at all — that is what distinguishes it from the
    // other two.
    const mesh::DynamicSurfaceStats after = surface->stats();
    CHECK(after.vertices == before.vertices);
    CHECK(after.edges == before.edges);
    CHECK(after.faces == before.faces);
    CHECK(after.halfedges == before.halfedges);
}

TEST_CASE("topology ops: a boundary and a constrained edge refuse to flip") {
    Mesh grid = plane_grid(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());

    for (EdgeId e : live_edges(*surface))
        if (surface->is_boundary_edge(e)) {
            // A boundary has one face and no second diagonal to flip to.
            CHECK(mesh::flip_edge(*surface, e, {}, nullptr, true).result ==
                  TopologyResult::Constrained);
            break;
        }

    for (EdgeId e : live_edges(*surface))
        if (!surface->is_boundary_edge(e)) {
            surface->edges_mutable().at(e).constraints |= EdgeConstraint::UvSeam;
            // A flip MOVES the edge, so a constrained edge flipping is the
            // constraint being deleted.
            CHECK(mesh::flip_edge(*surface, e, {}, nullptr, true).result ==
                  TopologyResult::Constrained);
            break;
        }
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology ops: thousands of interleaved operations keep every invariant") {
    // THE FUZZ RUN. Rare local connectivity failures do not surface any other
    // way: they need a sequence nobody would write by hand, and they show up as
    // a surface that is still plausible.
    //
    // Deterministic by construction — a fixed seed and a hand-rolled generator
    // rather than anything from a device — so a failure reproduces exactly.
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        CAPTURE(seed);
        Mesh base = (seed % 2) ? cube_sphere(3, 1.0f) : plane_grid(5, 1.0f);
        auto surface = DynamicSurface::from_mesh(base);
        REQUIRE(surface.has_value());

        Rng rng(seed);
        std::size_t splits = 0, collapses = 0, flips = 0, moves = 0;

        for (int step = 0; step < 400; ++step) {
            const std::vector<EdgeId> edges = live_edges(*surface);
            if (edges.empty()) break;
            const EdgeId e = edges[rng.below(static_cast<std::uint32_t>(edges.size()))];

            switch (rng.below(4)) {
                case 0:
                    if (mesh::split_edge(*surface, e, 0.25f + 0.5f * rng.unit()).result ==
                        TopologyResult::Ok)
                        ++splits;
                    break;
                case 1:
                    if (mesh::collapse_edge(*surface, e).result == TopologyResult::Ok)
                        ++collapses;
                    break;
                case 2:
                    if (mesh::flip_edge(*surface, e, {}, nullptr, rng.below(2) == 0).result ==
                        TopologyResult::Ok)
                        ++flips;
                    break;
                default: {
                    // A MOVE, because the geometric refusals only fire on a
                    // surface that is not flat, and a fuzz run that only ever
                    // changed connectivity would never reach them.
                    const mesh::HalfEdgeId h = surface->halfedge_of(e);
                    const mesh::VertexId v = surface->origin_of(h);
                    if (mesh::DynamicVertex* rec = surface->vertex(v)) {
                        rec->position =
                            rec->position + cf3((rng.unit() - 0.5f) * 0.1f,
                                                (rng.unit() - 0.5f) * 0.1f,
                                                (rng.unit() - 0.5f) * 0.1f);
                        ++moves;
                    }
                    break;
                }
            }

            // AFTER EVERY SINGLE OPERATION. Validating at the end would tell us
            // that something broke and nothing about which operation did it.
            const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
            CAPTURE(step);
            CAPTURE(report.summary());
            REQUIRE(report.ok);
        }

        // The run has to have DONE something, or it validates an untouched
        // surface four hundred times and reports success.
        CAPTURE(splits);
        CAPTURE(collapses);
        CAPTURE(flips);
        CHECK(splits > 0);
        CHECK(collapses > 0);
        CHECK(moves > 0);
        CHECK(surface->stats().faces > 0);
    }
}
