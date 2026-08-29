// A dab costs what it touches (dynamic-topology spec, add-dynamic-topology).
//
// THE GATE THAT DECIDES WHETHER THIS FEATURE IS USABLE. For a fixed brush
// footprint the stamp cost must stay in one band as the surface grows: a 50x
// model must not be a 50x stamp. Everything else here is correctness; this is
// the one that says a sculptor can work on a model too big to redraw.
//
// MEASURED IN WORK, NOT IN WALL-CLOCK. A timing on a shared machine is a
// measurement of the machine, and this suite runs on CI boxes doing other
// things. So the assertions are on what the stamp TOUCHED — vertices gathered,
// faces re-normalled, chunks dirtied, operators run — which is the quantity the
// requirement is actually about and which does not move when the box is busy.
// The wall-clock numbers at the sizes the requirement names are in
// `benchmarks/bench_main.cpp`, where a benchmark harness can control for it.
//
// AND THAT CHOICE HAS A KNOWN BLIND SPOT, which cost a real defect. An operator
// that reads the whole surface to find four half-edges still TOUCHES four, so
// every assertion in this file stayed green while a 320k-face stamp ran 570x a
// 20k-face one at an identical footprint. Work-counting cannot see a cost that
// is paid in reading rather than writing. The complement lives in
// `test_sculpt_allocation.cpp`, which asserts that a topology operator's
// APPETITE does not follow the surface either; the two together are the gate,
// and neither is sufficient alone.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/parallel/cancel.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;

namespace {

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

// A flat patch at a FIXED vertex spacing, `n` quads a side. Growing `n` grows
// the MODEL without changing how densely it is tessellated.
//
// THAT DISTINCTION IS THE WHOLE EXPERIMENT, and the first version of this file
// got it wrong. Subdividing one sphere more finely is not "a bigger model" — a
// fixed-radius brush covers a fixed fraction of a sphere's AREA, so a finer
// tessellation legitimately puts proportionally more vertices under it, and the
// ratio the test was checking is constant by construction whatever the
// implementation does. A sculptor's "bigger model" means more surface at the
// same detail, and that is what this builds.
Mesh flat_patch(int n, float spacing) {
    Mesh m;
    const float half = spacing * static_cast<float>(n) * 0.5f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + spacing * static_cast<float>(x), 0.0f,
                                      -half + spacing * static_cast<float>(z)));
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

struct Touched {
    std::size_t faces = 0;      // total in the model
    std::size_t moved = 0;      // vertices the stamp displaced
    std::size_t dirty_chunks = 0;
    std::size_t chunks = 0;
    std::size_t operations = 0;
};

Touched stamp_patch(int n, bool topology_on) {
    auto surface = DynamicSurface::from_mesh(flat_patch(n, 0.05f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    sculptor.bvh().clear_dirty();

    mesh::MeshBrushSettings brush;
    // A FIXED footprint on a surface of FIXED density: the same brush touching
    // the same number of vertices, on models of very different sizes.
    brush.center = cf3(0, 0, 0);
    brush.radius = 0.2f;
    brush.strength = 0.25f;

    DynamicTopologySettings topo;
    topo.enabled = topology_on;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 4.0f;

    const mesh::DynamicStampResult r = sculptor.stamp(mesh::MeshBrush::Draw, brush, topo);

    Touched out;
    out.faces = surface->stats().faces;
    out.moved = r.moved_vertices;
    out.dirty_chunks = sculptor.bvh().dirty_leaves().size();
    out.chunks = sculptor.bvh().leaf_count();
    out.operations = r.remesh.total();
    REQUIRE(mesh::validate_dynamic_surface(*surface).ok);
    return out;
}

}  // namespace

TEST_CASE("dynamic scale: a fixed footprint costs the same as the model grows") {
    // Three models at one density, each about four times the last.
    const Touched small = stamp_patch(40, /*topology_on=*/false);   // ~3k faces
    const Touched medium = stamp_patch(80, /*topology_on=*/false);  // ~13k
    const Touched large = stamp_patch(160, /*topology_on=*/false);  // ~51k

    CAPTURE(small.faces);
    CAPTURE(medium.faces);
    CAPTURE(large.faces);
    REQUIRE(large.faces > small.faces * 12);  // the models really do differ by 16x

    CAPTURE(small.moved);
    CAPTURE(medium.moved);
    CAPTURE(large.moved);
    REQUIRE(small.moved > 20);

    // THE BAND. The same brush on the same density reaches the same vertices,
    // whatever else the model contains. Exactly equal, in fact — the patch is
    // uniform, so the only reason this is a band rather than an equality is
    // that the rim of the brush can catch a vertex differently at an edge.
    CHECK(large.moved <= small.moved * 5 / 4);
    CHECK(large.moved * 5 / 4 >= small.moved);
    CHECK(medium.moved <= small.moved * 5 / 4);

    // ...and the dirtied chunks stay a handful while the index grows.
    CAPTURE(small.dirty_chunks);
    CAPTURE(small.chunks);
    CAPTURE(large.dirty_chunks);
    CAPTURE(large.chunks);
    REQUIRE(large.chunks > small.chunks * 4);
    CHECK(large.dirty_chunks <= small.dirty_chunks * 2 + 2);
    CHECK(large.dirty_chunks * 4 < large.chunks);
}

TEST_CASE("dynamic scale: with topology on, the work still follows the brush") {
    const Touched small = stamp_patch(40, /*topology_on=*/true);
    const Touched large = stamp_patch(160, /*topology_on=*/true);
    CAPTURE(small.operations);
    CAPTURE(large.operations);
    REQUIRE(small.operations > 0);

    // The remesher works to a BRUSH-RELATIVE target, so the number of
    // operations is set by the brush and the local density rather than by the
    // model: sixteen times the surface, the same amount of remeshing.
    CHECK(large.operations <= small.operations * 5 / 4);
    CHECK(large.operations * 5 / 4 >= small.operations);
}

TEST_CASE("dynamic scale: an ordinary dab rebuilds nothing global") {
    // ASSERTED BY INSTRUMENTATION rather than inferred from a timing. The
    // chunked index exposes its own structure, so "no whole-index rebuild"
    // is a statement about identities that can be checked exactly.
    auto surface = DynamicSurface::from_mesh(cube_sphere(64, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);

    const std::size_t chunks_before = sculptor.bvh().leaf_count();
    // Every chunk's revision before the stamp.
    std::vector<std::uint64_t> revisions;
    for (std::uint32_t i = 0; i < chunks_before; ++i) {
        const mesh::SurfaceLeaf* leaf = sculptor.bvh().leaf(i);
        revisions.push_back(leaf ? leaf->revision : 0);
    }
    sculptor.bvh().clear_dirty();

    mesh::MeshBrushSettings brush;
    brush.center = cf3(0, 0, 1);
    brush.radius = 0.1f;
    brush.strength = 0.2f;
    DynamicTopologySettings topo;
    topo.enabled = false;
    REQUIRE(sculptor.stamp(mesh::MeshBrush::Draw, brush, topo).moved_vertices > 0);

    // The chunk SET did not change — no rebuild — and only the chunks the brush
    // reached advanced their revision.
    CHECK(sculptor.bvh().leaf_count() == chunks_before);
    std::size_t advanced = 0;
    for (std::uint32_t i = 0; i < chunks_before; ++i) {
        const mesh::SurfaceLeaf* leaf = sculptor.bvh().leaf(i);
        if (leaf && leaf->revision != revisions[i]) ++advanced;
    }
    CAPTURE(advanced);
    CAPTURE(chunks_before);
    CHECK(advanced > 0);
    CHECK(advanced * 4 < chunks_before);
    CHECK(advanced == sculptor.bvh().dirty_leaves().size());
}

TEST_CASE("dynamic scale: memory is reported per element rather than estimated") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(32, 1.0f));
    REQUIRE(surface.has_value());
    const mesh::DynamicSurfaceStats stats = surface->stats();
    const std::size_t bytes = surface->bytes();
    REQUIRE(stats.faces > 0);

    const double per_face = static_cast<double>(bytes) / static_cast<double>(stats.faces);
    CAPTURE(bytes);
    CAPTURE(stats.faces);
    CAPTURE(per_face);
    // A half-edge surface costs several times a flat mesh per triangle — four
    // pools, two half-edges an edge — and that is the price of the
    // representation rather than a defect. What matters is that it is REPORTED,
    // and that it is a few hundred bytes rather than a few thousand.
    CHECK(per_face > 16.0);
    CHECK(per_face < 512.0);

    // Dead slots are reported separately, because a surface never compacts and
    // a long session's pools grow with its history rather than its content.
    // A freshly imported surface has none.
    CHECK(stats.dead_slots == 0);

    DynamicSculptor sculptor(*surface);
    mesh::MeshBrushSettings brush;
    brush.center = cf3(0, 0, 1);
    brush.radius = 0.3f;
    brush.strength = 0.3f;
    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 8.0f;
    sculptor.stamp(mesh::MeshBrush::Draw, brush, topo);
    // ...and after an edit that collapses as well as splits, some appear.
    CHECK(surface->bytes() > bytes);

    // The index's own cost is reported too, so a memory budget is not blind to
    // the structure that makes the stamps fast.
    CHECK(sculptor.bvh().bytes() > 0);
}

TEST_CASE("dynamic scale: a cancelled conversion leaves nothing half-built") {
    // BUILD-THEN-PUBLISH. A half-built half-edge structure is one that
    // validates in some places and crashes a walk in others, so a cancelled
    // conversion must produce nothing at all rather than a partial surface.
    const Mesh source = cube_sphere(48, 1.0f);
    parallel::CancelToken token;
    token.cancel();

    mesh::DynamicBuildError err = mesh::DynamicBuildError::None;
    auto cancelled = DynamicSurface::from_mesh(source, {}, &err, &token);
    CHECK_FALSE(cancelled.has_value());

    // ...and the same call without the token succeeds, so the refusal is the
    // cancel rather than the input.
    token.reset();
    auto fine = DynamicSurface::from_mesh(source, {}, &err, &token);
    REQUIRE(fine.has_value());
    CHECK(mesh::validate_dynamic_surface(*fine).ok);

    // A cancelled EXPORT returns an empty mesh rather than a partial one: a
    // caller that ignored the cancel and drew the result would draw a fraction
    // of the model, which is worse than drawing nothing.
    parallel::CancelToken export_token;
    export_token.cancel();
    const Mesh partial = fine->to_mesh({}, &export_token);
    CHECK(partial.indices.empty());
    CHECK(partial.positions.empty());

    const Mesh whole = fine->to_mesh();
    CHECK(whole.indices.size() == fine->stats().faces * 3);
}
