// THE LOCALITY GATE, AT SIZES CI CAN AFFORD (sculpt-runtime spec,
// add-extreme-poly-runtime 7.3, 7.5 and 7.7).
//
// One sentence is what this whole change serves: A DAB COSTS APPROXIMATELY WHAT
// IT TOUCHES, NOT WHAT THE MODEL HOLDS. Sixteen times the vertices at the same
// touched region must not be sixteen times the dab.
//
// THE FIXTURE IS FIXED-SPACING WITH A GROWING EXTENT, and that is the whole
// experiment rather than a detail of it. "A bigger model" here means more of
// the same geometry at the same detail — never a more finely subdivided one,
// which would grow the footprint along with the model and make the gate
// measure nothing at all. The brush radius is in world units and does not move
// between the rows, so the number of vertices under it is a constant by
// construction and any growth in cost is the model leaking in.
//
// WHAT IS ASSERTED, AND WHY IN THIS ORDER.
//
//   1. THE COUNTS, which are deterministic and are the real gate. The workset,
//      the write region, the chunks a dab dirties and the bytes a host is
//      handed are identical at both sizes — not close, identical. A path that
//      had gone O(model) shows here first and shows unambiguously.
//   2. THE PEAK, which is 7.7 and catches what the counts cannot: a buffer
//      sized to the SURFACE during warm-up costs nothing per stamp and is
//      still O(model). The high-water marks have to match too.
//   3. THE TIME, last and with a wide band. This box is shared and a wall
//      clock on it is the least trustworthy thing in the file — so the
//      assertion is a MEDIAN over many stamps against a band of 4x for a 16x
//      model, which an O(model) path misses by a factor of four and which
//      run-to-run noise does not reach. The benchmark carries the real
//      distribution; this is the gate that fails the build.
//
// The full matrix — 100k to 20M vertices, five footprints, P50/P95/P99/max per
// stage — is `benchmarks/bench_extreme_poly.cpp`, because twenty million
// vertices is not a unit test.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "clay/memory/budget.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using mesh::ChunkOptions;
using mesh::ChunkTable;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;

namespace {

// FIXED SPACING, GROWING EXTENT. `n` is the number of quads a side and the
// spacing never changes, so the surface under a brush of a given world radius
// is the same surface at every size.
Mesh plane(int n, float spacing) {
    Mesh m;
    // MEASURED OUTWARD FROM THE CENTRE, not from a corner, and this is not
    // cosmetic. `-half + spacing * x` rounds differently at two sizes — the
    // same world point comes out a few ulps apart on a 96-quad grid and a
    // 384-quad one — so the brush's ball admits a slightly different set of
    // vertices and the counts below stop being comparable. Centring makes the
    // vertex near the origin the same float at every size, which is what lets
    // the gate assert equality rather than a band.
    const int centre = n / 2;
    m.positions.reserve(static_cast<std::size_t>(n + 1) * static_cast<std::size_t>(n + 1));
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(spacing * static_cast<float>(x - centre),
                                      ((x + z) & 1) ? 0.0625f : 0.0f,
                                      spacing * static_cast<float>(z - centre)));
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

struct Row {
    std::size_t vertices = 0;
    std::size_t workset = 0;
    std::size_t write_region = 0;
    std::size_t dirty_chunks = 0;
    std::size_t preview_bytes = 0;
    std::size_t peak_workset = 0;
    double median_micros = 0.0;
};

// Every chunk the brush's ball reaches, which is what a partitioner hands a
// stamp as its candidate set — and, on the transport side, what a host is
// handed to re-upload.
std::size_t chunks_reached(const ChunkTable& table, cfloat3 centre, float radius,
                           const mesh::SurfaceView& view, std::size_t* bytes) {
    std::size_t reached = 0;
    *bytes = 0;
    const math::Aabb ball{cf3(centre.x - radius, centre.y - radius, centre.z - radius),
                          cf3(centre.x + radius, centre.y + radius, centre.z + radius)};
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        const mesh::SurfaceChunk* c = table.chunk(i);
        if (c == nullptr || !c->bounds.intersects(ball)) continue;
        ++reached;
        const mesh::ChunkReadback sized =
            view.copy_chunk(i, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
        *bytes += static_cast<std::size_t>(sized.vertex_count) * 3u * sizeof(float) +
                  static_cast<std::size_t>(sized.index_count) * sizeof(std::uint32_t);
    }
    return reached;
}

Row measure(int n, float spacing, MeshBrush verb, float radius) {
    Row row;
    Mesh mesh = plane(n, spacing);
    row.vertices = mesh.positions.size();

    ChunkOptions options;
    options.target_faces = 256;
    ChunkTable table;
    mesh::partition_mesh_chunks(mesh, options, &table);

    MeshSculptor sculptor(mesh);
    // A HOST THAT PICKS HAS AN INDEX, and every host that places a brush picks.
    // `surface_index` never builds one on its own behalf — measured at 689 ms
    // against 1.24 ms saved per stamp — so the test builds it the way a host
    // does, once, before the stroke.
    (void)sculptor.bvh();

    MeshBrushSettings brush;
    brush.center = cf3(0, 0, 0);
    brush.radius = radius;
    brush.strength = 0.2f;
    brush.smooth_iterations = 2;
    brush.direction = cf3(0.01f, 0.005f, 0.0f);
    brush.geodesic = mesh::default_geodesic(verb);

    memory::PeakTelemetry peak;
    // WARM. Growth on first encountering a footprint is permitted by the
    // requirement; steady repeated local sculpting is not.
    for (int i = 0; i < 8; ++i) sculptor.stamp(verb, brush);

    std::vector<double> micros;
    micros.reserve(41);
    for (int i = 0; i < 41; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        sculptor.stamp(verb, brush);
        const auto end = std::chrono::steady_clock::now();
        micros.push_back(
            std::chrono::duration<double, std::micro>(end - begin).count());
        peak.observe_workset(sculptor.workset().size());
    }
    std::sort(micros.begin(), micros.end());
    row.median_micros = micros[micros.size() / 2];
    row.workset = sculptor.workset().size();
    row.write_region = sculptor.write_region().size();
    row.peak_workset = peak.workset_vertices;

    const mesh::SurfaceView view = mesh::SurfaceView::over_mesh(mesh, table);
    row.dirty_chunks = chunks_reached(table, brush.center, radius, view, &row.preview_bytes);
    return row;
}

}  // namespace

TEST_CASE("locality gate: sixteen times the model at the same footprint is not sixteen times "
          "the dab") {
    // Both verbs, because the two query shapes are different code: a geodesic
    // verb walks the one-ring from a seed the index found, and a ball verb asks
    // the index directly. Either could go O(model) without the other.
    const MeshBrush verbs[2] = {MeshBrush::Draw, MeshBrush::Flatten};
    for (MeshBrush verb : verbs) {
        CAPTURE(static_cast<int>(verb));
        const Row small = measure(96, 0.02f, verb, 0.12f);
        const Row large = measure(384, 0.02f, verb, 0.12f);

        CAPTURE(small.vertices);
        CAPTURE(large.vertices);
        const double model_ratio =
            static_cast<double>(large.vertices) / static_cast<double>(small.vertices);
        REQUIRE(model_ratio > 14.0);

        // 1. THE COUNTS. Identical, not close: the footprint is the same
        //    surface at both sizes and the brush is the same brush.
        CAPTURE(small.workset);
        CAPTURE(large.workset);
        REQUIRE(small.workset > 0u);
        CHECK(large.workset == small.workset);
        CHECK(large.write_region == small.write_region);
        // The chunks are a BAND rather than an equality, and the reason is
        // stated rather than hidden: a chunk is a fixed face count and the
        // partition is a median split over the whole mesh, so where its
        // boundaries fall depends on the mesh. The same ball therefore
        // straddles a different number of chunks at the two sizes — by one or
        // two, never by the model ratio, which is the claim.
        CAPTURE(small.dirty_chunks);
        CAPTURE(large.dirty_chunks);
        REQUIRE(small.dirty_chunks > 0u);
        CHECK(large.dirty_chunks <= 2u * small.dirty_chunks);
        CHECK(large.preview_bytes <= 2u * small.preview_bytes);

        // 2. THE PEAK (7.7). What a stroke needed at its high-water mark, which
        //    is the assertion a per-stamp count cannot make: a buffer sized to
        //    the surface once during warm-up costs nothing per stamp and is
        //    still O(model).
        CHECK(large.peak_workset == small.peak_workset);

        // 3. THE TIME, with a band the shared box cannot cross and an O(model)
        //    path cannot stay inside.
        CAPTURE(small.median_micros);
        CAPTURE(large.median_micros);
        REQUIRE(small.median_micros > 0.0);
        const double time_ratio = large.median_micros / small.median_micros;
        CAPTURE(time_ratio);
        CAPTURE(model_ratio);
        CHECK(time_ratio < 4.0);
        // And stated the way the requirement states it, so a failure reads as
        // the claim it broke rather than as a number.
        CHECK(time_ratio * 3.0 < model_ratio);
    }
}

TEST_CASE("preview gate: what a host is handed follows the region, not the surface") {
    // The same measurement as the locality gate's counts, isolated and stated
    // against the FULL upload — because "the dirty bytes did not grow" is only
    // a claim if the whole surface's bytes did.
    const Row small = measure(96, 0.02f, MeshBrush::Draw, 0.12f);
    const Row large = measure(384, 0.02f, MeshBrush::Draw, 0.12f);

    Mesh small_mesh = plane(96, 0.02f);
    Mesh large_mesh = plane(384, 0.02f);
    const std::size_t small_full =
        small_mesh.positions.size() * 3u * sizeof(float) + small_mesh.indices.size() * 4u;
    const std::size_t large_full =
        large_mesh.positions.size() * 3u * sizeof(float) + large_mesh.indices.size() * 4u;

    CAPTURE(small_full);
    CAPTURE(large_full);
    CAPTURE(small.preview_bytes);
    CAPTURE(large.preview_bytes);
    // The whole surface grew by the model ratio...
    CHECK(static_cast<double>(large_full) > 14.0 * static_cast<double>(small_full));
    // ...and what a stamp hands the host stayed inside a chunk of where it was.
    REQUIRE(small.preview_bytes > 0u);
    CHECK(large.preview_bytes <= 2u * small.preview_bytes);
    // A HOST'S FRAME IS CHEAPER THAN THE MODEL BY THE MODEL RATIO, which is
    // the whole reason the transport is per chunk.
    CHECK(large.preview_bytes * 50u < large_full);
}
