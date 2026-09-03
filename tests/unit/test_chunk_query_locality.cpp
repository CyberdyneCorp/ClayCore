// THE NO-SCAN GATE (sculpt-runtime spec, add-extreme-poly-runtime 3.1).
//
// The requirement is one sentence: "brush volume -> top-level tree -> candidate
// chunks -> candidate vertices -> exact footprint. NEVER A SCAN OVER EVERY
// VERTEX." It held on the fixed mesh and the adaptive surface, both of which
// descend a `Bvh`, and it was FALSE on a multires level — nothing picks against
// a level, so nothing builds a ray tree for one, and every unseeded stamp
// resolved its anchor by walking the level's whole class space.
//
// WHY THIS IS COUNTED AND NOT TIMED. The region a stamp produces is IDENTICAL
// whether its anchor was found by a descent or by a scan — that is the entire
// design, and `test_mesh_sculpt_parity.cpp` holds it to the byte. So there is
// nothing in the output to assert on: the only observable difference is how much
// work was done to get there, and on a shared box a wall clock is the least
// trustworthy instrument in the tree. `MeshSculptor::anchor_measurements()`
// counts class positions actually measured, across all three paths — the chunk
// descent's candidates, the scan's whole class space, and the walk's own
// internal scan when it had to find its own seed.
//
// THE FIXTURE IS FIXED-SPACING WITH A GROWING EXTENT, the same construction and
// for the same reason as `test_extreme_poly_scaling.cpp`: "a bigger model" must
// mean more of the same geometry at the same detail, never a more finely
// subdivided one, or the footprint grows with the model and the gate measures
// nothing.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "clay/mesh/sculpt.h"
#include "clay/mesh/surface_chunks.h"
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

// Centred on the origin so the vertex near it is the same float at every size;
// see the note in test_extreme_poly_scaling.cpp for why that matters to a gate
// that asserts equality rather than a band.
Mesh plane(int n, float spacing) {
    Mesh m;
    const int centre = n / 2;
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

MeshBrushSettings dab(cfloat3 centre) {
    MeshBrushSettings s;
    s.center = centre;
    s.radius = 0.25f;
    s.strength = 0.25f;
    s.geodesic = true;
    return s;
}

struct Run {
    std::size_t vertices = 0;
    std::size_t measurements = 0;
    std::size_t moved = 0;
    std::uint64_t digest = 0;
};

// A short stroke, so the gate covers the dabs AFTER the first as well: a design
// that resolved the first anchor cheaply and scanned for the rest would pass a
// one-dab test.
Run stroke(int n, bool with_chunks) {
    Mesh m = plane(n, 0.03125f);
    ChunkTable table;
    mesh::partition_mesh_chunks(m, ChunkOptions{}, &table);
    MeshSculptor sculptor(m);
    if (with_chunks) sculptor.set_chunks(&table);
    Run r;
    r.vertices = m.positions.size();
    for (int step = 0; step < 8; ++step) {
        const float t = static_cast<float>(step) * 0.02f;
        r.moved += sculptor.stamp(MeshBrush::Draw, dab(cf3(t - 0.08f, 0.0f, 0.0f)));
    }
    r.measurements = sculptor.anchor_measurements();
    // A cheap order-dependent digest of the surface, so the two rows can be
    // compared for byte-identity without a second harness.
    std::uint64_t h = 1469598103934665603ull;
    for (const cfloat3& p : m.positions) {
        const float xs[3] = {p.x, p.y, p.z};
        for (float f : xs) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            h = (h ^ bits) * 1099511628211ull;
        }
    }
    r.digest = h;
    return r;
}

}  // namespace

TEST_CASE("chunk query: anchoring a dab stops following the model") {
    // Sixteen times the vertices at the same world footprint.
    const Run small = stroke(96, true);
    const Run big = stroke(384, true);

    REQUIRE(small.vertices > 0);
    CHECK(big.vertices > small.vertices * 15);

    // THE GATE. Not a band and not a ratio: the ball is the same ball over the
    // same geometry, so the chunks it admits and the vertices in them are the
    // same count at both sizes. A path that had gone O(model) shows here
    // unambiguously and by the model ratio, not by a few percent.
    CHECK(big.measurements == small.measurements);
    // And it is a SMALL number, not merely a stable one: a gate that only
    // compared the two rows would pass a runtime that scanned both models
    // identically badly.
    CHECK(small.measurements < small.vertices);

    // The dab still did its work at both sizes; a gate over a stroke that
    // reached nothing would assert on two zeroes.
    CHECK(small.moved > 0);
    CHECK(big.moved == small.moved);

    // The numbers this gate was proven on, so a future reader can tell a
    // regression from a re-tuning. With the chunk tree the two rows measure the
    // SAME count; with `chunk_index()` forced to null the revert COMPILES and
    // the same two rows read 75,272 and 1,185,800 — which is 9,409 and 148,225
    // vertices times the eight dabs, one whole class scan per dab, and a ratio
    // of 15.75 against a model ratio of 16.
    MESSAGE("anchor measurements: " << small.measurements << " at " << small.vertices
                                    << " vertices, " << big.measurements << " at "
                                    << big.vertices);
}

TEST_CASE("chunk query: the descent returns exactly what the scan returns") {
    // THE EXACTNESS CLAIM, and the reason the gate above has to count rather
    // than measure the output: these two runs differ in how the anchor was
    // found and in nothing else, so the surfaces must agree to the BYTE. If
    // they ever do not, the chunk path is not an acceleration of the scan, it
    // is a second brush.
    for (int n : {96, 384}) {
        const Run chunked = stroke(n, true);
        const Run scanned = stroke(n, false);
        CHECK(chunked.digest == scanned.digest);
        CHECK(chunked.moved == scanned.moved);
        // ... and the point of the whole exercise: the same answer for far less
        // work. The scan measures the class space once per dab; the descent
        // measures the neighbourhood.
        CHECK(chunked.measurements < scanned.measurements);
    }
}

TEST_CASE("chunk query: a stamp publishes its own dirty chunks") {
    // Task 4: the dirty stream is a property of the sculptor, not of every
    // host. Before this, `bench_extreme_poly` and the scaling test each marked
    // chunks from OUTSIDE after a stamp, which is host-side logic every host
    // would have to copy and copy identically.
    Mesh m = plane(96, 0.03125f);
    ChunkTable table;
    mesh::partition_mesh_chunks(m, ChunkOptions{}, &table);
    MeshSculptor sculptor(m);
    sculptor.set_chunks(&table);

    // Read BEFORE the stamp: the partition advances topology on every chunk it
    // creates, so "unchanged by the stamp" is the only form of the claim that
    // means anything.
    std::vector<std::uint64_t> topology_before(table.slot_count(), 0);
    std::vector<std::uint64_t> geometry_before(table.slot_count(), 0);
    for (std::uint32_t i = 0; i < table.slot_count(); ++i)
        if (const mesh::SurfaceChunk* c = table.chunk(i)) {
            topology_before[i] = c->revisions.topology;
            geometry_before[i] = c->revisions.geometry;
        }

    const std::size_t moved = sculptor.stamp(MeshBrush::Draw, dab(cf3(0, 0, 0)));
    REQUIRE(moved > 0);
    REQUIRE(!sculptor.dirty_chunks().empty());

    // Every chunk the sculptor named holds at least one vertex it actually
    // wrote, so the stream is not merely non-empty but correct: a host
    // re-uploads what changed and not a superset of it.
    std::vector<char> written(m.positions.size(), 0);
    for (mesh::WorkItemId item : sculptor.write_region()) {
        std::size_t n = 0;
        const std::uint32_t* members =
            sculptor.adjacency().members(item.as_weld_class(), &n);
        for (std::size_t i = 0; i < n; ++i) written[members[i]] = 1;
    }
    bool before_geometry_advanced = true;
    bool topology_before_eq = true;
    for (std::uint32_t id : sculptor.dirty_chunks()) {
        const mesh::SurfaceChunk* c = table.chunk(id);
        if (c == nullptr) continue;
        if (c->revisions.geometry <= geometry_before[id]) before_geometry_advanced = false;
        if (c->revisions.topology != topology_before[id]) topology_before_eq = false;
        const mesh::ChunkVertexSpan span = table.vertices(id);
        bool any = false;
        for (std::size_t i = 0; i < span.size(); ++i)
            if (span[i] < written.size() && written[span[i]]) any = true;
        CHECK(any);
    }

    // Geometry, never topology: a fixed sculptor cannot change `indices`, so a
    // host told to re-upload an index buffer would be re-uploading what it
    // already has. Asserted as "the stamp did not ADVANCE it" rather than as
    // "it is zero", because partitioning the table is itself a topology change
    // and leaves a revision behind — the first version of this check asserted
    // zero and read 64, which is the partition and not the stamp.
    CHECK(before_geometry_advanced);
    CHECK(topology_before_eq);
}
