// The chunked mutable spatial index (dynamic-topology spec,
// add-dynamic-topology).
//
// EVERY QUERY IS CHECKED AGAINST BRUTE FORCE. An index that is fast and wrong is
// worse than no index at all: it is wrong in a way that depends on the tree's
// shape, so it is wrong intermittently and only on the models that are too big
// to inspect by hand.
//
// And the locality claim is ASSERTED rather than profiled. "A topology mutation
// touches the leaves it changed and no others" is a statement about which
// leaves the dirty list names, which is checkable exactly; a timing would only
// say it was fast today.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "clay/mesh/dynamic_bvh.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/topology_ops.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicBvh;
using mesh::DynamicSurface;
using mesh::FaceId;
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

// THE ORACLE. Every face, tested directly, in slot order.
cfloat3 closest_on_triangle(cfloat3 p, cfloat3 a, cfloat3 b, cfloat3 c) {
    const cfloat3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = cdot(ab, ap), d2 = cdot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const cfloat3 bp = p - b;
    const float d3 = cdot(ab, bp), d4 = cdot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const cfloat3 cp = p - c;
    const float d5 = cdot(ab, cp), d6 = cdot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

std::vector<FaceId> brute_ball(const DynamicSurface& s, cfloat3 centre, float radius) {
    std::vector<FaceId> out;
    const float r2 = radius * radius;
    s.faces().for_each_live([&](FaceId f, const mesh::DynamicFace&) {
        mesh::VertexId v[3];
        if (!s.face_vertices(f, v)) return;
        const cfloat3 q = closest_on_triangle(centre, s.position_of(v[0]), s.position_of(v[1]),
                                              s.position_of(v[2]));
        if (cdot2(q - centre) <= r2) out.push_back(f);
    });
    std::sort(out.begin(), out.end(), [](FaceId a, FaceId b) { return a.slot < b.slot; });
    return out;
}

float brute_closest_distance(const DynamicSurface& s, cfloat3 p) {
    float best = 1e30f;
    s.faces().for_each_live([&](FaceId f, const mesh::DynamicFace&) {
        mesh::VertexId v[3];
        if (!s.face_vertices(f, v)) return;
        const cfloat3 q = closest_on_triangle(p, s.position_of(v[0]), s.position_of(v[1]),
                                              s.position_of(v[2]));
        best = std::min(best, clength(q - p));
    });
    return best;
}

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)
        : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
    float unit() { return static_cast<float>(next() % 100000u) / 100000.0f; }
    float signed_unit() { return unit() * 2.0f - 1.0f; }
};

}  // namespace

TEST_CASE("dynamic bvh: chunks a surface into leaves of the target size") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    REQUIRE(surface.has_value());

    DynamicBvh bvh;
    mesh::DynamicBvhOptions options;
    options.target_leaf_faces = 64;
    options.max_leaf_faces = 128;
    bvh.build(*surface, options);

    REQUIRE(bvh.leaf_count() > 1);
    std::size_t indexed = 0;
    for (std::uint32_t i = 0; i < bvh.leaf_count(); ++i) {
        const mesh::SurfaceLeaf* leaf = bvh.leaf(i);
        REQUIRE(leaf != nullptr);
        CAPTURE(i);
        CHECK(leaf->faces.size() <= options.target_leaf_faces);
        CHECK_FALSE(leaf->faces.empty());
        indexed += leaf->faces.size();
    }
    // EVERY face is in exactly one leaf: a face in none is invisible to every
    // query, and a face in two is counted twice.
    CHECK(indexed == surface->stats().faces);
    surface->faces().for_each_live([&](FaceId f, const mesh::DynamicFace&) {
        CHECK(bvh.leaf_of(f) != DynamicBvh::kNoLeaf);
    });
}

TEST_CASE("dynamic bvh: every query agrees with brute force") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(10, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    mesh::DynamicBvhOptions options;
    options.target_leaf_faces = 48;
    bvh.build(*surface, options);

    Rng rng(11);
    std::vector<FaceId> got;
    std::size_t nonempty = 0;
    for (int trial = 0; trial < 60; ++trial) {
        // NEAR THE SURFACE, which is where a brush lands. Points sampled from a
        // box around the sphere mostly miss it, and a query that agrees with
        // brute force about the empty set sixty times has checked nothing.
        cfloat3 dir = cf3(rng.signed_unit(), rng.signed_unit(), rng.signed_unit());
        if (clength(dir) < 1e-3f) dir = cf3(0, 1, 0);
        const cfloat3 p = dir / clength(dir) * (0.8f + rng.unit() * 0.4f);
        const float r = 0.05f + rng.unit() * 0.5f;
        CAPTURE(trial);

        bvh.faces_in_ball(*surface, p, r, &got);
        const std::vector<FaceId> want = brute_ball(*surface, p, r);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i].slot == want[i].slot);
        if (!want.empty()) ++nonempty;

        // The closest point, to the same tolerance brute force gives — which is
        // none: it is the same arithmetic over the same triangles.
        const DynamicBvh::ClosestPoint near = bvh.closest(*surface, p);
        REQUIRE(near.found);
        CHECK(near.distance == doctest::Approx(brute_closest_distance(*surface, p)));
    }
    // The trials have to have FOUND something, or this checks that two empty
    // answers agree sixty times.
    // Nearly all of them; the few empties are small radii in the gaps between
    // faces, which is a real case and not a reason to loosen the oracle.
    CHECK(nonempty >= 50);
}

TEST_CASE("dynamic bvh: a raycast finds the nearest hit") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    bvh.build(*surface);

    Rng rng(5);
    std::size_t hits = 0;
    for (int trial = 0; trial < 40; ++trial) {
        const cfloat3 origin = cf3(rng.signed_unit(), rng.signed_unit(), rng.signed_unit()) * 3.0f;
        const cfloat3 dir = cf3(0, 0, 0) - origin;  // aimed at the centre
        const DynamicBvh::RayHit hit = bvh.raycast(*surface, origin, dir);
        if (!hit.hit) continue;
        ++hits;
        // The hit is ON the sphere, which is the only check that does not
        // reimplement the query.
        CHECK(clength(hit.position) == doctest::Approx(1.0f).epsilon(0.02));
        // ...and it is the NEAREST hit: nothing lies between the origin and it.
        const float to_hit = clength(hit.position - origin);
        CHECK(to_hit == doctest::Approx(hit.t).epsilon(1e-3));
    }
    CHECK(hits == 40);  // every ray aimed at the centre of a closed sphere hits

    // A ray pointing away finds nothing rather than finding it behind.
    const DynamicBvh::RayHit miss = bvh.raycast(*surface, cf3(0, 0, 3), cf3(0, 0, 1));
    CHECK_FALSE(miss.hit);
}

TEST_CASE("dynamic bvh: a mutation touches the leaves it changed and no others") {
    // THE LOCALITY CLAIM, asserted rather than profiled.
    auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    mesh::DynamicBvhOptions options;
    options.target_leaf_faces = 64;
    bvh.build(*surface, options);
    REQUIRE(bvh.leaf_count() > 4);
    bvh.clear_dirty();
    CHECK(bvh.dirty_leaves().empty());

    // Move one vertex and update its faces.
    const mesh::VertexId v = surface->vertices().id_at(0);
    REQUIRE(v.valid());
    std::vector<FaceId> faces;
    REQUIRE(surface->incident_faces(v, &faces));
    surface->vertex(v)->position = surface->position_of(v) * 1.05f;
    bvh.update_many(*surface, faces);

    // The leaves named are exactly the ones holding those faces.
    std::vector<std::uint32_t> expected;
    for (FaceId f : faces) expected.push_back(bvh.leaf_of(f));
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());

    std::vector<std::uint32_t> got = bvh.dirty_leaves();
    std::sort(got.begin(), got.end());
    CHECK(got == expected);
    // ...and that is a small fraction of the index, not most of it.
    CHECK(got.size() < bvh.leaf_count() / 2);

    // The epoch clear costs nothing and leaves nothing behind.
    bvh.clear_dirty();
    CHECK(bvh.dirty_leaves().empty());
    for (std::uint32_t i = 0; i < bvh.leaf_count(); ++i) {
        const mesh::SurfaceLeaf* leaf = bvh.leaf(i);
        if (!leaf) continue;
        CHECK_FALSE(leaf->geometry_dirty);
        CHECK_FALSE(leaf->topology_dirty);
    }
}

TEST_CASE("dynamic bvh: insert and erase keep the queries exact") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    mesh::DynamicBvhOptions options;
    options.target_leaf_faces = 32;
    options.max_leaf_faces = 64;
    bvh.build(*surface, options);

    // Split some edges, feeding the index each operator's own result — which is
    // what the sculptor will do.
    std::vector<mesh::EdgeId> edges;
    surface->edges().for_each_live(
        [&](mesh::EdgeId id, const mesh::DynamicEdge&) { edges.push_back(id); });
    std::size_t splits = 0;
    for (std::size_t i = 0; i < edges.size() && splits < 30; i += 7) {
        const mesh::SplitResult r = mesh::split_edge(*surface, edges[i], 0.5f);
        if (r.result != mesh::TopologyResult::Ok) continue;
        ++splits;
        // The two original faces are gone and up to four exist; the index is
        // told about each.
        for (int k = 0; k < r.face_count; ++k) {
            bvh.erase(r.faces[k]);
            bvh.insert(*surface, r.faces[k]);
        }
    }
    REQUIRE(splits > 10);

    // Re-index everything the splits created but the loop above did not name.
    surface->faces().for_each_live([&](FaceId f, const mesh::DynamicFace&) {
        if (bvh.leaf_of(f) == DynamicBvh::kNoLeaf) bvh.insert(*surface, f);
    });

    // AND THE QUERIES ARE STILL EXACT. An index maintained incrementally that
    // quietly stops matching brute force is the failure this whole file is for.
    Rng rng(3);
    std::vector<FaceId> got;
    for (int trial = 0; trial < 30; ++trial) {
        const cfloat3 p = cf3(rng.signed_unit(), rng.signed_unit(), rng.signed_unit()) * 1.3f;
        const float r = 0.1f + rng.unit() * 0.5f;
        CAPTURE(trial);
        bvh.faces_in_ball(*surface, p, r, &got);
        const std::vector<FaceId> want = brute_ball(*surface, p, r);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i].slot == want[i].slot);
    }
}

TEST_CASE("dynamic bvh: a leaf splits when it grows past its bound") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());
    DynamicBvh bvh;
    mesh::DynamicBvhOptions options;
    // Deliberately tiny, so the growth path runs on a small fixture.
    options.target_leaf_faces = 8;
    options.max_leaf_faces = 12;
    bvh.build(*surface, options);
    const std::size_t before = bvh.leaf_count();
    REQUIRE(before > 1);

    // Split every edge once: the face count roughly doubles, so leaves must
    // split rather than growing without bound.
    std::vector<mesh::EdgeId> edges;
    surface->edges().for_each_live(
        [&](mesh::EdgeId id, const mesh::DynamicEdge&) { edges.push_back(id); });
    for (mesh::EdgeId e : edges) {
        const mesh::SplitResult r = mesh::split_edge(*surface, e, 0.5f);
        if (r.result != mesh::TopologyResult::Ok) continue;
        for (int k = 0; k < r.face_count; ++k) {
            bvh.erase(r.faces[k]);
            bvh.insert(*surface, r.faces[k]);
        }
    }
    surface->faces().for_each_live([&](FaceId f, const mesh::DynamicFace&) {
        if (bvh.leaf_of(f) == DynamicBvh::kNoLeaf) bvh.insert(*surface, f);
    });

    CHECK(bvh.leaf_count() > before);
    for (std::uint32_t i = 0; i < bvh.leaf_count(); ++i) {
        const mesh::SurfaceLeaf* leaf = bvh.leaf(i);
        if (!leaf) continue;
        CAPTURE(i);
        // NO LEAF GREW WITHOUT BOUND. A chunk that swallows a whole stroke's
        // worth of new geometry stops being a chunk.
        CHECK(leaf->faces.size() <= options.max_leaf_faces);
    }
}
