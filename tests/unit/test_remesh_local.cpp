// The local adaptive remesher: convergence, hysteresis and preservation
// (dynamic-topology spec, add-dynamic-topology).
//
// The two claims that matter are hard to check by inspection and easy to check
// by measurement, which is why they are here rather than in a comment:
//
//   - a stretched patch CONVERGES toward the target edge length without the
//     surface drifting, and
//   - a stationary brush does not oscillate one edge between split and
//     collapse for as long as the artist holds it there.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/dynamic_bvh.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/remesh_local.h"
#include "clay/mesh/topology_ops.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicBvh;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;

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

struct LengthStats {
    float mean = 0.0f;
    float max = 0.0f;
    float min = 0.0f;
    std::size_t count = 0;
};

LengthStats edge_lengths(const DynamicSurface& s) {
    LengthStats out;
    out.min = 1e30f;
    float sum = 0.0f;
    s.edges().for_each_live([&](mesh::EdgeId e, const mesh::DynamicEdge&) {
        const float len = s.edge_length(e);
        sum += len;
        out.max = std::max(out.max, len);
        out.min = std::min(out.min, len);
        ++out.count;
    });
    if (out.count) out.mean = sum / static_cast<float>(out.count);
    return out;
}

// How far the surface moved, as the largest distance from any vertex to the
// plane it started on. The remesher may add and remove vertices freely; what it
// may NOT do is push the surface somewhere else.
float drift_from_plane(const DynamicSurface& s) {
    float worst = 0.0f;
    s.vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        worst = std::max(worst, std::fabs(v.position.y));
    });
    return worst;
}

}  // namespace

TEST_CASE("remesh: a stretched patch converges toward the target") {
    // A patch whose edges are far too long for the target, remeshed repeatedly.
    Mesh grid = plane_grid(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    bvh.build(*surface);

    DynamicTopologySettings settings;
    settings.detail_mode = mesh::DynamicDetailMode::World;
    settings.target_edge_length = 0.25f;
    settings.max_ops_per_stamp = 4000;
    settings.relax_after_remesh = false;  // measured separately

    const LengthStats start = edge_lengths(*surface);
    CAPTURE(start.mean);
    REQUIRE(start.mean > settings.target_edge_length * 1.5f);

    for (int pass = 0; pass < 8; ++pass) {
        const mesh::RemeshStats stats =
            mesh::remesh_region(*surface, &bvh, cf3(0, 0, 0), 3.0f, settings);
        CAPTURE(pass);
        CAPTURE(stats.split);
        CAPTURE(stats.collapsed);
        const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
        CAPTURE(report.summary());
        REQUIRE(report.ok);
    }

    const LengthStats end = edge_lengths(*surface);
    CAPTURE(end.mean);
    CAPTURE(end.max);
    // CONVERGED: the mean is near the target and nothing is wildly long.
    CHECK(end.mean < start.mean);
    CHECK(end.mean < settings.target_edge_length * 1.4f);
    CHECK(end.max < settings.target_edge_length * settings.split_factor * 1.5f);
    CHECK(end.count > start.count * 2);

    // AND THE SURFACE DID NOT MOVE. Every new vertex is a midpoint of two
    // vertices on the plane, so the plane is where they all stay; a remesher
    // that drifted would be changing the model rather than its triangulation.
    CHECK(drift_from_plane(*surface) < 1e-5f);
}

TEST_CASE("remesh: hysteresis stops a stationary brush oscillating") {
    // THE DEFECT THIS PREVENTS: with one threshold, an edge just above it
    // splits into two just below it, which collapse back into one just above
    // it, for as long as the artist holds the brush still.
    auto surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    bvh.build(*surface);

    DynamicTopologySettings settings;
    settings.detail_mode = mesh::DynamicDetailMode::World;
    settings.target_edge_length = 0.18f;
    settings.relax_after_remesh = false;

    // Let it settle first.
    for (int i = 0; i < 6; ++i) mesh::remesh_region(*surface, &bvh, cf3(0, 0, 1), 0.7f, settings);
    const std::size_t settled_faces = surface->stats().faces;

    // Now hold the brush still. Once converged, further stamps must do almost
    // nothing — and in particular must not split and collapse the same edges
    // over and over, which would show as a steady stream of both.
    std::size_t churn = 0;
    for (int i = 0; i < 6; ++i) {
        const mesh::RemeshStats stats =
            mesh::remesh_region(*surface, &bvh, cf3(0, 0, 1), 0.7f, settings);
        churn += stats.split + stats.collapsed;
    }
    CAPTURE(churn);
    CAPTURE(settled_faces);
    // A handful of operations as the last stragglers settle is fine; a stream
    // proportional to the region is the oscillation.
    CHECK(churn < settled_faces / 8);

    // ...and the face count is stable rather than pumping up and down.
    const std::size_t after = surface->stats().faces;
    const std::size_t diff =
        after > settled_faces ? after - settled_faces : settled_faces - after;
    CHECK(diff < settled_faces / 8);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("remesh: brush-relative detail makes a smaller brush finer") {
    // The whole point of the mode: a sculptor shrinking the brush to add detail
    // gets finer geometry, without reaching for a second slider.
    auto coarse_surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
    auto fine_surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
    REQUIRE(coarse_surface.has_value());
    REQUIRE(fine_surface.has_value());
    DynamicBvh coarse_bvh, fine_bvh;
    coarse_bvh.build(*coarse_surface);
    fine_bvh.build(*fine_surface);

    DynamicTopologySettings settings;
    settings.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    settings.detail_resolution = 6.0f;
    settings.relax_after_remesh = false;

    for (int i = 0; i < 4; ++i) {
        mesh::remesh_region(*coarse_surface, &coarse_bvh, cf3(0, 0, 1), 0.8f, settings);
        mesh::remesh_region(*fine_surface, &fine_bvh, cf3(0, 0, 1), 0.25f, settings);
    }

    // The small brush touched a smaller area and made it much finer, so measure
    // the edges NEAR the brush rather than over the whole sphere.
    auto mean_near = [](const DynamicSurface& s, cfloat3 centre, float radius) {
        float sum = 0.0f;
        std::size_t n = 0;
        s.edges().for_each_live([&](mesh::EdgeId e, const mesh::DynamicEdge&) {
            if (clength(s.edge_midpoint(e) - centre) > radius) return;
            sum += s.edge_length(e);
            ++n;
        });
        return n ? sum / static_cast<float>(n) : 0.0f;
    };
    const float coarse_mean = mean_near(*coarse_surface, cf3(0, 0, 1), 0.2f);
    const float fine_mean = mean_near(*fine_surface, cf3(0, 0, 1), 0.2f);
    CAPTURE(coarse_mean);
    CAPTURE(fine_mean);
    CHECK(fine_mean < coarse_mean);
    CHECK(mesh::validate_dynamic_surface(*fine_surface).ok);
}

TEST_CASE("remesh: a boundary is preserved and never closed") {
    // THE CASE AN UNCONSTRAINED COLLAPSE GETS WRONG: a patch whose border would
    // shrink away under repeated collapses, taking the surface's shape with it.
    Mesh grid = plane_grid(6, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    bvh.build(*surface);
    const std::size_t border_before = surface->stats().boundary_edges;
    REQUIRE(border_before == 24);

    // A target far LARGER than every edge, so the collapse pass wants to
    // collapse everything it is allowed to.
    DynamicTopologySettings settings;
    settings.detail_mode = mesh::DynamicDetailMode::World;
    settings.target_edge_length = 2.0f;
    settings.allow_split = false;
    settings.relax_after_remesh = false;
    settings.preserve_boundaries = true;

    for (int i = 0; i < 6; ++i) mesh::remesh_region(*surface, &bvh, cf3(0, 0, 0), 3.0f, settings);

    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    REQUIRE(report.ok);

    // The border is exactly as long as it was: not one of its edges collapsed.
    CHECK(surface->stats().boundary_edges == border_before);
    // ...and the patch still spans what it spanned, which is what "the boundary
    // is preserved" means to somebody looking at it.
    float extent = 0.0f;
    surface->vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        extent = std::max(extent, std::max(std::fabs(v.position.x), std::fabs(v.position.z)));
    });
    CHECK(extent == doctest::Approx(1.0f));
}

TEST_CASE("remesh: a seam and a crease survive the pass") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    bvh.build(*surface);

    // Mark a band of edges as a seam and another as sharp.
    std::size_t seams = 0, sharps = 0;
    surface->edges_mutable().for_each_live_mutable([&](mesh::EdgeId, mesh::DynamicEdge& e) {
        if (seams < 12) {
            e.constraints |= mesh::EdgeConstraint::UvSeam;
            ++seams;
        } else if (sharps < 12) {
            e.constraints |= mesh::EdgeConstraint::Sharp;
            ++sharps;
        }
    });
    REQUIRE(seams == 12);
    REQUIRE(sharps == 12);

    DynamicTopologySettings settings;
    settings.detail_mode = mesh::DynamicDetailMode::World;
    settings.target_edge_length = 2.0f;  // collapse everything permitted
    settings.allow_split = false;
    settings.relax_after_remesh = false;

    for (int i = 0; i < 5; ++i) mesh::remesh_region(*surface, &bvh, cf3(0, 0, 0), 3.0f, settings);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);

    // THE FEATURE SURVIVES; the edge COUNT may fall by one or two, and the
    // difference between those two statements is worth being exact about.
    //
    // A constrained edge is never collapsed directly — the blocker mask stops
    // it, and the assertion below re-states that. But a collapse BESIDE one
    // merges the two remaining edges of the dying triangle into a single edge
    // carrying the union of their constraints. So two seam edges either side of
    // a collapsed one become one seam edge: the seam curve is unbroken and one
    // edge shorter. Requiring the count never to fall would be requiring the
    // remesher never to work next to a seam.
    std::size_t seams_after = 0, sharps_after = 0;
    surface->edges().for_each_live([&](mesh::EdgeId, const mesh::DynamicEdge& e) {
        if (mesh::has_constraint(e.constraints, mesh::EdgeConstraint::UvSeam)) ++seams_after;
        if (mesh::has_constraint(e.constraints, mesh::EdgeConstraint::Sharp)) ++sharps_after;
    });
    CAPTURE(seams_after);
    CAPTURE(sharps_after);
    CHECK(seams_after * 4 >= seams * 3);
    CHECK(sharps_after * 4 >= sharps * 3);

    // ...and no constrained edge can be collapsed directly, which is the rule
    // the merge above operates under.
    mesh::TopologyOpOptions op;
    op.collapse_blockers = mesh::EdgeConstraint::UvSeam | mesh::EdgeConstraint::Sharp;
    std::size_t refused = 0, tried = 0;
    std::vector<mesh::EdgeId> constrained;
    surface->edges().for_each_live([&](mesh::EdgeId id, const mesh::DynamicEdge& e) {
        if (e.constraints != 0) constrained.push_back(id);
    });
    for (mesh::EdgeId e : constrained) {
        if (!surface->live(e)) continue;
        ++tried;
        if (mesh::collapse_edge(*surface, e, op).result == mesh::TopologyResult::Constrained)
            ++refused;
    }
    REQUIRE(tried > 0);
    CHECK(refused == tried);
}

TEST_CASE("remesh: the operation budget is a bound the caller sets") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    bvh.build(*surface);

    DynamicTopologySettings settings;
    settings.detail_mode = mesh::DynamicDetailMode::World;
    settings.target_edge_length = 0.02f;  // far finer than the surface
    settings.max_ops_per_stamp = 40;
    settings.relax_after_remesh = false;

    const mesh::RemeshStats stats =
        mesh::remesh_region(*surface, &bvh, cf3(0, 0, 0), 3.0f, settings);
    // A BOUND, so a host can trade detail for latency rather than choosing
    // between the two the library picked.
    CHECK(stats.total() <= 40);
    CHECK(stats.hit_budget);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("remesh: relax evens the spacing without moving the surface") {
    Mesh grid = plane_grid(8, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    // Push the interior vertices around in the plane, so the spacing is uneven
    // and the surface itself is still flat.
    std::uint32_t i = 0;
    surface->vertices_mutable().for_each_live_mutable(
        [&](mesh::VertexId, mesh::DynamicVertex& v) {
            // CAST BEFORE SUBTRACTING. `i % 5` is unsigned, so `- 2` on it
            // wraps to four billion rather than going negative, and the jitter
            // becomes 1.3e8 — which is how the first run of this test managed to
            // report an edge-length spread of 1.8e8 on a unit plane.
            const float jitter = (static_cast<int>(i++ % 5) - 2) * 0.03f;
            if (std::fabs(v.position.x) < 0.9f && std::fabs(v.position.z) < 0.9f)
                v.position = cf3(v.position.x + jitter, 0.0f, v.position.z - jitter);
        });
    surface->refresh_all_normals();

    DynamicBvh bvh;
    bvh.build(*surface);
    const LengthStats before = edge_lengths(*surface);

    DynamicTopologySettings settings;
    settings.preserve_boundaries = true;
    for (int pass = 0; pass < 12; ++pass)
        mesh::relax_region(*surface, &bvh, cf3(0, 0, 0), 3.0f, 0.3f, settings);

    const LengthStats after = edge_lengths(*surface);
    // Counts unchanged: relax redistributes, it does not remesh.
    CHECK(after.count == before.count);
    // The spread narrowed, which is what "evened the spacing" means.
    CHECK(after.max - after.min < before.max - before.min);
    // TANGENTIAL ONLY: the plane is still the plane.
    CHECK(drift_from_plane(*surface) < 1e-5f);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("remesh: each verb's timing is chosen, not shared") {
    // The defaults are recorded per verb WITH a reason, so a verb whose timing
    // is wrong is a decision to revisit rather than an oversight.
    CHECK(mesh::default_timing(mesh::MeshBrush::Grab) == mesh::RemeshTiming::AfterBrush);
    CHECK(mesh::default_timing(mesh::MeshBrush::Snakehook) == mesh::RemeshTiming::BeforeAndAfter);
    CHECK(mesh::default_timing(mesh::MeshBrush::Clay) == mesh::RemeshTiming::BeforeBrush);
    CHECK(mesh::default_timing(mesh::MeshBrush::Crease) == mesh::RemeshTiming::BeforeBrush);
    CHECK(mesh::default_timing(mesh::MeshBrush::Smooth) == mesh::RemeshTiming::BeforeBrush);
    CHECK(mesh::default_timing(mesh::MeshBrush::Relax) == mesh::RemeshTiming::AfterBrush);
}
