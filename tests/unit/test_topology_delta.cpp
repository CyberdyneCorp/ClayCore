// A topology gesture is one sparse, reversible undo step (dynamic-topology
// spec, add-dynamic-topology).
//
// The bar is BIT-EXACT: reverting a stroke that split, collapsed and flipped
// many edges gives back the surface that was there, connectivity included. Not
// "a surface with the same shape" — the same surface, so that a handle held by
// the spatial index or a host's upload buffer still names what it named.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/topology_delta.h"
#include "clay/mesh/topology_ops.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSurface;
using mesh::EdgeId;
using mesh::Mesh;
using mesh::TopologyDelta;
using mesh::TopologyResult;

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

// A fingerprint of the whole surface, slot by slot: every LIVE element, its
// slot, its generation, and exactly what it holds. Comparing exported meshes
// would compare the SHAPE and miss the thing that matters — that the handles
// still name what they named.
//
// DEAD SLOTS ARE SKIPPED, deliberately. A pool never compacts, so a gesture
// that created and reverted elements leaves the storage longer than it found
// it, with dead slots on the end. That is the design working, not a difference
// in the surface, and a fingerprint that counted it would call every correct
// revert a failure — which is what the first draft of this function did.
std::uint64_t fingerprint(const DynamicSurface& s) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    auto u32 = [&mix](std::uint32_t v) { mix(&v, sizeof(v)); };

    for (std::uint32_t i = 0; i < s.vertices().capacity_slots(); ++i) {
        const mesh::VertexId id = s.vertices().id_at(i);
        if (!id.valid()) continue;
        u32(i);
        u32(id.generation);
        const mesh::DynamicVertex& v = s.vertices().at(id);
        mix(&v.position, sizeof(v.position));
        mix(&v.normal, sizeof(v.normal));
        mix(&v.color, sizeof(v.color));
        mix(&v.mask, sizeof(v.mask));
        u32(v.outgoing.slot);
        u32(v.outgoing.generation);
    }
    for (std::uint32_t i = 0; i < s.halfedges().capacity_slots(); ++i) {
        const mesh::HalfEdgeId id = s.halfedges().id_at(i);
        if (!id.valid()) continue;
        u32(i);
        u32(id.generation);
        const mesh::DynamicHalfEdge& e = s.halfedges().at(id);
        u32(e.origin.slot);
        u32(e.face.slot);
        u32(e.next.slot);
        u32(e.twin.slot);
        u32(e.edge.slot);
        mix(&e.uv, sizeof(e.uv));
    }
    for (std::uint32_t i = 0; i < s.edges().capacity_slots(); ++i) {
        const mesh::EdgeId id = s.edges().id_at(i);
        if (!id.valid()) continue;
        u32(i);
        u32(id.generation);
        u32(s.edges().at(id).halfedge.slot);
        u32(s.edges().at(id).constraints);
    }
    for (std::uint32_t i = 0; i < s.faces().capacity_slots(); ++i) {
        const mesh::FaceId id = s.faces().id_at(i);
        if (!id.valid()) continue;
        u32(i);
        u32(id.generation);
        u32(s.faces().at(id).halfedge.slot);
        mix(&s.faces().at(id).normal, sizeof(kernel::cfloat3));
    }
    return h;
}

std::vector<EdgeId> live_edges(const DynamicSurface& s) {
    std::vector<EdgeId> out;
    s.edges().for_each_live([&](EdgeId id, const mesh::DynamicEdge&) { out.push_back(id); });
    return out;
}

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)
        : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n) { return n ? next() % n : 0; }
    float unit() { return static_cast<float>(next() % 100000u) / 100000.0f; }
};

}  // namespace

TEST_CASE("topology delta: one split reverts bit-exactly") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(3, 1.0f));
    REQUIRE(surface.has_value());
    const std::uint64_t before = fingerprint(*surface);

    TopologyDelta delta;
    const EdgeId e = live_edges(*surface).front();
    REQUIRE(mesh::split_edge(*surface, e, 0.5f, {}, &delta).result == TopologyResult::Ok);
    CHECK(fingerprint(*surface) != before);
    CHECK_FALSE(delta.empty());

    REQUIRE(delta.revert(*surface));
    CHECK(fingerprint(*surface) == before);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology delta: revert and apply are both idempotent") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(3, 1.0f));
    REQUIRE(surface.has_value());
    const std::uint64_t start = fingerprint(*surface);

    TopologyDelta delta;
    const EdgeId e = live_edges(*surface).front();
    REQUIRE(mesh::split_edge(*surface, e, 0.5f, {}, &delta).result == TopologyResult::Ok);
    const std::uint64_t after = fingerprint(*surface);

    // Reverting twice is reverting once.
    delta.revert(*surface);
    CHECK(fingerprint(*surface) == start);
    delta.revert(*surface);
    CHECK(fingerprint(*surface) == start);

    // ...and re-applying gets back exactly where the operator left it, which is
    // what makes redo a real operation rather than a re-execution.
    delta.apply(*surface);
    CHECK(fingerprint(*surface) == after);
    delta.apply(*surface);
    CHECK(fingerprint(*surface) == after);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("topology delta: a whole gesture reverts as one step") {
    // THE REAL CASE: many operations of many kinds into ONE record, reverted
    // once. A stroke is hundreds of stamps and thousands of operations, and the
    // artist pressing undo means all of it.
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        auto surface = DynamicSurface::from_mesh(cube_sphere(3, 1.0f));
        REQUIRE(surface.has_value());
        const std::uint64_t before = fingerprint(*surface);

        TopologyDelta delta;
        Rng rng(seed);
        std::size_t applied = 0;
        for (int step = 0; step < 60; ++step) {
            const std::vector<EdgeId> edges = live_edges(*surface);
            if (edges.empty()) break;
            const EdgeId e = edges[rng.below(static_cast<std::uint32_t>(edges.size()))];
            switch (rng.below(3)) {
                case 0:
                    if (mesh::split_edge(*surface, e, 0.5f, {}, &delta).result ==
                        TopologyResult::Ok)
                        ++applied;
                    break;
                case 1:
                    if (mesh::collapse_edge(*surface, e, {}, &delta).result == TopologyResult::Ok)
                        ++applied;
                    break;
                default:
                    if (mesh::flip_edge(*surface, e, {}, &delta, true).result ==
                        TopologyResult::Ok)
                        ++applied;
                    break;
            }
        }
        CAPTURE(applied);
        REQUIRE(applied > 8);
        CHECK(fingerprint(*surface) != before);

        REQUIRE(delta.revert(*surface));
        CHECK(fingerprint(*surface) == before);
        const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
        CAPTURE(report.summary());
        CHECK(report.ok);
    }
}

TEST_CASE("topology delta: the record follows the elements touched, not the operations") {
    // COALESCED. A gesture that goes back over its own ground records each
    // element ONCE, keeping the first `before` and the last `after`. That is
    // the difference between an undo step and a memory leak on a stroke that
    // works one small region for a long time.
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());

    // An edge that CAN flip: not every one can, and the first in slot order
    // happens to sit on a valence-3 vertex, where flipping would leave a
    // two-edge fan and the operator refuses on its own.
    TopologyDelta probe;
    EdgeId flippable;
    for (EdgeId candidate : live_edges(*surface))
        if (mesh::flip_edge(*surface, candidate, {}, &probe, true).result == TopologyResult::Ok) {
            probe.revert(*surface);
            flippable = candidate;
            break;
        }
    REQUIRE(flippable.valid());

    TopologyDelta delta;
    // Flip the same edge back and forth many times. Every flip touches the same
    // handful of elements.
    std::size_t flips = 0;
    for (int i = 0; i < 40; ++i)
        if (mesh::flip_edge(*surface, flippable, {}, &delta, true).result == TopologyResult::Ok)
            ++flips;
    REQUIRE(flips > 20);

    // Forty operations over a dozen elements is a dozen entries, not forty
    // times a dozen.
    CHECK(delta.element_count() < 40);
    CHECK(delta.element_count() > 0);
}

TEST_CASE("topology delta: bytes round-trip and a hostile record is refused") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(3, 1.0f));
    REQUIRE(surface.has_value());
    const std::uint64_t before = fingerprint(*surface);

    TopologyDelta delta;
    for (int i = 0; i < 5; ++i) {
        const std::vector<EdgeId> edges = live_edges(*surface);
        mesh::split_edge(*surface, edges[static_cast<std::size_t>(i)], 0.5f, {}, &delta);
    }
    const std::vector<std::uint8_t> bytes = delta.encode();
    REQUIRE(bytes.size() > 16);

    TopologyDelta back;
    REQUIRE(TopologyDelta::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.element_count() == delta.element_count());
    // The decoded record reverts the surface exactly as the original would,
    // which is the only property of a decoded record that matters.
    REQUIRE(back.revert(*surface));
    CHECK(fingerprint(*surface) == before);

    // A TRUNCATED OR HOSTILE RECORD IS REFUSED BEFORE ANYTHING IS ALLOCATED.
    // Sizing a vector from a count the buffer could not hold is how a reader
    // gets asked for a gigabyte.
    TopologyDelta rejected;
    CHECK_FALSE(TopologyDelta::decode(bytes.data(), 8, &rejected));
    CHECK_FALSE(TopologyDelta::decode(nullptr, 0, &rejected));
    for (std::size_t cut = 16; cut < bytes.size(); cut += 37)
        CHECK_FALSE(TopologyDelta::decode(bytes.data(), cut, &rejected));

    // A count field rewritten to something enormous.
    std::vector<std::uint8_t> hostile = bytes;
    hostile[8] = 0xff;
    hostile[9] = 0xff;
    hostile[10] = 0xff;
    hostile[11] = 0x7f;
    CHECK_FALSE(TopologyDelta::decode(hostile.data(), hostile.size(), &rejected));

    // A version this build does not know is refused rather than read as a
    // prefix of itself.
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 99;
    CHECK_FALSE(TopologyDelta::decode(newer.data(), newer.size(), &rejected));
}

// -- the pool's own invariant, which the surface's does not cover -------------
//
// THE SURFACE CAN BE PERFECT AND THE POOL BROKEN. Reverting and re-applying a
// gesture restored, on the run that found this: every slot, every generation,
// every liveness flag, every half-edge link, every position, colour, mask and
// UV -- and left every free list CYCLIC and holding hundreds of live slots.
// `validate_dynamic_surface` passed on both sides, because it walks the
// connectivity and the connectivity was right.
//
// It surfaced as the NEXT DAB NEVER RETURNING. `SlotPool::create` walks the
// free list to skip slots an undo revived, so a cycle there is an infinite
// loop inside the following allocation rather than a wrong answer anywhere.
//
// These assert the invariant DIRECTLY and with REQUIRE, so a regression fails
// in milliseconds instead of hanging a CI job until it is killed.

namespace {

// Every free list of a surface names exactly its dead slots, once each.
void check_free_lists(const DynamicSurface& s, const char* when) {
    std::size_t visited = 0, live_on_list = 0;
    bool cycle = false;
    CAPTURE(when);
    REQUIRE(s.vertices().free_list_intact(&visited, &cycle, &live_on_list));
    REQUIRE(s.halfedges().free_list_intact(&visited, &cycle, &live_on_list));
    REQUIRE(s.edges().free_list_intact(&visited, &cycle, &live_on_list));
    REQUIRE(s.faces().free_list_intact(&visited, &cycle, &live_on_list));
}

}  // namespace

TEST_CASE("topology delta: a replay leaves every free list intact") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());
    check_free_lists(*surface, "before the gesture");

    // A gesture that creates AND destroys slots, which is what makes the two
    // directions of the replay touch the same slots from both ends.
    TopologyDelta delta;
    std::size_t splits = 0, collapses = 0;
    for (EdgeId e : live_edges(*surface)) {
        if (splits >= 12) break;
        if (mesh::split_edge(*surface, e, 0.5f, {}, &delta).result == TopologyResult::Ok)
            ++splits;
    }
    for (EdgeId e : live_edges(*surface)) {
        if (collapses >= 8) break;
        if (!surface->live(e)) continue;
        if (mesh::collapse_edge(*surface, e, {}, &delta).result == TopologyResult::Ok)
            ++collapses;
    }
    REQUIRE(splits > 4);
    REQUIRE(collapses > 2);
    check_free_lists(*surface, "after the gesture");

    REQUIRE(delta.revert(*surface));
    check_free_lists(*surface, "after revert");
    REQUIRE(mesh::validate_dynamic_surface(*surface).ok);

    REQUIRE(delta.apply(*surface));
    check_free_lists(*surface, "after apply");
    REQUIRE(mesh::validate_dynamic_surface(*surface).ok);

    // And again, because the replay is idempotent and a fix that only holds
    // for one round is not a fix.
    REQUIRE(delta.revert(*surface));
    check_free_lists(*surface, "after second revert");
    REQUIRE(delta.apply(*surface));
    check_free_lists(*surface, "after second apply");
}

TEST_CASE("topology delta: a surface still allocates after a replay") {
    // THE END-TO-END SHAPE, and the one a user hits: undo, redo, keep
    // sculpting. Every allocation below went through the corrupted free list
    // before the fix, and the first one did not return.
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());

    TopologyDelta delta;
    std::size_t splits = 0;
    for (EdgeId e : live_edges(*surface)) {
        if (splits >= 16) break;
        if (mesh::split_edge(*surface, e, 0.5f, {}, &delta).result == TopologyResult::Ok)
            ++splits;
    }
    REQUIRE(splits > 8);

    REQUIRE(delta.revert(*surface));
    REQUIRE(delta.apply(*surface));
    // The invariant first, so a regression trips here rather than spinning in
    // the loop below.
    check_free_lists(*surface, "after replay");

    // Now allocate. Splitting takes a vertex, two half-edges, an edge and two
    // faces per operation, so this exercises every pool's free list.
    std::size_t after = 0;
    for (EdgeId e : live_edges(*surface)) {
        if (after >= 24) break;
        if (!surface->live(e)) continue;
        if (mesh::split_edge(*surface, e, 0.5f).result == TopologyResult::Ok) ++after;
    }
    CHECK(after >= 24);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    check_free_lists(*surface, "after allocating again");
}
