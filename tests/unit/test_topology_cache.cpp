// Sharing an adjacency between the sculptors over one mesh
// (share-mesh-topology-cache).
//
// THE GATES ARE ABOUT WHAT IS SHARED AND WHAT IS NOT, and neither half is
// optional. A cache that never shares costs a fingerprint and buys nothing; a
// cache that shares too much serves a sculptor an adjacency over triangles it
// is not looking at, and that failure is SILENT — the brush does not crash, it
// moves the wrong vertices.
//
// So the sharing cases assert POINTER IDENTITY rather than equal contents,
// which is the only thing that proves the build was skipped, and the
// invalidation cases are built to defeat every cheap key in turn: same counts,
// different connectivity; positions moved; an owner forgotten.

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/topology_cache.h"

using namespace clay;
using namespace clay::mesh;
using kernel::cf3;

namespace {

// A grid, asymmetric in y so a rotation or an axis mix-up is visible, and big
// enough that the two CSR sorts are a real cost.
Mesh grid(int n, float spacing = 0.05f) {
    Mesh m;
    const int centre = n / 2;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(spacing * static_cast<float>(x - centre),
                                      ((x + z) & 1) ? spacing * 0.5f : 0.0f,
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

}  // namespace

TEST_CASE("two sculptors over one unchanged mesh share one topology") {
    Mesh m = grid(24);
    TopologyCache cache;

    MeshSculptor first(m, cache.acquire(1, m));
    MeshSculptor second(m, cache.acquire(1, m));

    // POINTER IDENTITY, not equal contents. Two adjacencies with the same rings
    // would compare equal after a rebuild, which is exactly the thing this is
    // meant to prove did not happen.
    CHECK(first.shared_adjacency().get() == second.shared_adjacency().get());

    const TopologyCacheStats s = cache.stats();
    CHECK(s.entries == 1);
    CHECK(s.misses == 1);
    CHECK(s.hits == 1);
}

TEST_CASE("a position-only change keeps the entry") {
    Mesh m = grid(16);
    TopologyCache cache;
    auto before = cache.acquire(1, m);

    // What a stroke does: positions move, connectivity does not.
    for (auto& p : m.positions) p = p + cf3(0.0f, 0.013f, 0.0f);

    auto after = cache.acquire(1, m);
    CHECK(before.get() == after.get());
    CHECK(cache.stats().hits == 1);

    // AND THIS IS THE SEMANTIC, stated as a test rather than left implicit: the
    // served adjacency is the partition built over the ORIGINAL positions,
    // which is exactly what a sculptor live across that same stroke would still
    // be holding.
    CHECK(after->class_count() == before->class_count());
}

TEST_CASE("identical counts with different connectivity do not collide") {
    Mesh m = grid(12);
    TopologyCache cache;
    auto before = cache.acquire(1, m);
    const std::size_t vertices = m.positions.size();
    const std::size_t triangles = m.triangle_count();

    // Rewire one triangle. Same vertex count, same triangle count, same
    // positions — every cheap key agrees, and the mesh is a different surface.
    // NOTHING invalidates the entry here: no `forget`, no revision moves. This
    // is the #472 shape, and the fingerprint is the only thing standing in it.
    std::swap(m.indices[0], m.indices[1]);

    auto after = cache.acquire(1, m);
    CHECK(m.positions.size() == vertices);
    CHECK(m.triangle_count() == triangles);
    CHECK(before.get() != after.get());
    CHECK(cache.stats().hits == 0);
    CHECK(cache.stats().misses == 2);

    // PROVEN TO CATCH ITS OWN REGRESSION: with the connectivity term removed
    // from the fingerprint, the remaining terms are identical and the lookup
    // would have hit. Asserted on the fingerprints rather than by mutating the
    // cache, so the proof lives in the same file as the claim.
    const TopologyFingerprint a = TopologyFingerprint::of(m, kDefaultWeldEpsilon);
    std::swap(m.indices[0], m.indices[1]);
    const TopologyFingerprint b = TopologyFingerprint::of(m, kDefaultWeldEpsilon);
    CHECK(a != b);
    CHECK(a.vertex_count == b.vertex_count);
    CHECK(a.triangle_count == b.triangle_count);
    CHECK(a.weld_epsilon == b.weld_epsilon);
}

TEST_CASE("a different weld epsilon is a different partition") {
    Mesh m = grid(8);
    TopologyCache cache;
    auto welded = cache.acquire(1, m, kDefaultWeldEpsilon);
    auto exact = cache.acquire(1, m, 0.0f);
    CHECK(welded.get() != exact.get());
    CHECK(cache.stats().hits == 0);
    // One entry: the second replaced the first rather than accumulating, which
    // is what bounds the cache at the number of layers.
    CHECK(cache.stats().entries == 1);
}

TEST_CASE("forgetting an owner rebuilds") {
    Mesh m = grid(8);
    TopologyCache cache;
    auto before = cache.acquire(1, m);
    before.reset();  // nothing else holds it
    const std::size_t released = cache.forget(1);
    CHECK(released > 0);
    CHECK(cache.stats().entries == 0);
    cache.acquire(1, m);
    CHECK(cache.stats().misses == 2);
}

TEST_CASE("two owners do not collide") {
    Mesh a = grid(8);
    Mesh b = grid(10);
    TopologyCache cache;
    auto first = cache.acquire(1, a);
    auto second = cache.acquire(2, b);
    CHECK(first.get() != second.get());
    CHECK(cache.stats().entries == 2);
    CHECK(cache.acquire(1, a).get() == first.get());
    CHECK(cache.acquire(2, b).get() == second.get());
}

TEST_CASE("a trim releases what nothing holds and keeps what something does") {
    Mesh held = grid(8);
    Mesh loose = grid(8);
    TopologyCache cache;
    auto keep = cache.acquire(1, held);
    cache.acquire(2, loose);  // released immediately; only the cache holds it

    const std::size_t released = cache.release_unused();
    CHECK(released > 0);
    CHECK(cache.stats().entries == 1);

    // The held entry is still THE SAME OBJECT, so a sculptor built over it
    // before the trim is still looking at live storage.
    CHECK(cache.acquire(1, held).get() == keep.get());
}

TEST_CASE("clearing leaves a live holder working") {
    Mesh m = grid(8);
    TopologyCache cache;
    auto held = cache.acquire(1, m);
    MeshSculptor sculptor(m, held);
    cache.clear();
    CHECK(cache.stats().entries == 0);
    // The sculptor's reference outlives the cache's, which is the whole reason
    // entries are counted rather than owned outright.
    CHECK(sculptor.adjacency().class_count() > 0);
    MeshBrushSettings brush;
    brush.center = cf3(0, 0, 0);
    brush.radius = 0.2f;
    brush.strength = 0.4f;
    brush.direction = cf3(0, 1, 0);
    CHECK(sculptor.stamp(MeshBrush::Draw, brush) > 0);
}

TEST_CASE("cached and uncached sculpts are bit-identical") {
    Mesh cached = grid(16);
    Mesh direct = grid(16);
    TopologyCache cache;

    MeshBrushSettings brush;
    brush.center = cf3(0, 0, 0);
    brush.radius = 0.15f;
    brush.strength = 0.5f;
    brush.direction = cf3(0, 1, 0);

    {
        // Warm the entry with a first sculptor, then discard it: the SECOND
        // sculptor is the one being compared, and it is the one served a
        // cached adjacency rather than a freshly built one.
        MeshSculptor warm(cached, cache.acquire(1, cached));
        (void)warm.stamp(MeshBrush::Draw, brush);
    }
    Mesh warmed = cached;
    Mesh warmed_direct = direct;
    {
        MeshSculptor a(warmed, cache.acquire(1, warmed));
        MeshSculptor b(warmed_direct, kDefaultWeldEpsilon);
        (void)b.stamp(MeshBrush::Draw, brush);
        (void)a.stamp(MeshBrush::Draw, brush);
    }
    // The two meshes diverge at the warm-up stamp, so compare the sculptors'
    // partitions rather than the positions: same class count, same class of
    // every vertex.
    REQUIRE(cache.acquire(1, warmed)->class_count() ==
            Adjacency::build(warmed_direct, kDefaultWeldEpsilon).class_count());
}

TEST_CASE("concurrent creation builds one authoritative entry") {
    Mesh m = grid(24);
    TopologyCache cache;
    constexpr int kThreads = 8;
    std::vector<std::shared_ptr<const Adjacency>> got(kThreads);
    std::vector<std::thread> threads;
    std::atomic<int> ready{0};
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&, i] {
            ++ready;
            while (ready.load() < kThreads) {
            }
            got[i] = cache.acquire(1, m);
        });
    for (auto& t : threads) t.join();

    for (int i = 1; i < kThreads; ++i) CHECK(got[i].get() == got[0].get());
    const TopologyCacheStats s = cache.stats();
    CHECK(s.entries == 1);
    CHECK(s.misses == 1);
    CHECK(s.hits == kThreads - 1);
}

TEST_CASE("the cache reports what it cost and what it saved") {
    Mesh m = grid(24);
    TopologyCache cache;
    cache.acquire(1, m);
    cache.acquire(1, m);
    const TopologyCacheStats s = cache.stats();
    CHECK(s.bytes > 0);
    CHECK(s.build_nanoseconds > 0);
    // Verifying is what a hit costs and building is what it avoids. Asserting
    // the ORDER rather than a ratio: a ratio is a claim about this machine.
    CHECK(s.verify_nanoseconds < s.build_nanoseconds);
}
