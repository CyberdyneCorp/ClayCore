// Dirty-chunk tracking and the regional mesh (#86 part 2).
//
// The load-bearing test here is the seam one. Per-chunk meshing is only
// tractable because greedy quads are axis-aligned and exact: clamping the
// merge to a chunk boundary splits a quad rather than cracking the surface. A
// triangle-count comparison would not prove that — it would pass just as well
// if a whole row of faces went missing at the seam — so the comparison below
// decomposes both meshes back into UNIT FACES and asserts the two sets are
// equal, face for face and colour for colour.
//
// The other half is the dirty set, whose most likely failure is a missed
// neighbour across a chunk face: a hole in the surface that appears only at
// chunk boundaries, on a grid large enough to have any.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using voxel::BrushParams;
using voxel::BrushShape;
using voxel::kChunkDim;
using voxel::VoxelChunkMeshRange;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

// A unit face of the meshed surface: which cell face it is, so two meshes of
// the same surface produce the same set however their quads were merged.
struct UnitFace {
    int axis, sign;
    int a, u, v;
    bool operator<(const UnitFace& o) const {
        return std::array<int, 5>{axis, sign, a, u, v} <
               std::array<int, 5>{o.axis, o.sign, o.a, o.u, o.v};
    }
};

int lround_i(float f) { return static_cast<int>(std::lround(f)); }

// Decompose a greedy mesh back into unit faces. Every quad is 4 vertices and 6
// indices in emit order, so the vertices alone carry the rectangle; the normal
// says which face direction and the colour rides along. Cell size is 1 in
// every fixture here, so the coordinates come back exact.
std::map<UnitFace, cfloat3> unit_faces(const mesh::Mesh& m) {
    std::map<UnitFace, cfloat3> faces;
    REQUIRE(m.positions.size() % 4 == 0);
    REQUIRE(m.indices.size() == m.positions.size() / 4 * 6);
    for (std::size_t q = 0; q * 4 < m.positions.size(); ++q) {
        const cfloat3 n = m.normals[q * 4];
        const int axis = n.x != 0 ? 0 : (n.y != 0 ? 1 : 2);
        const int sign = (axis == 0 ? n.x : (axis == 1 ? n.y : n.z)) > 0 ? 1 : -1;
        const cfloat3 p0 = m.positions[q * 4], p2 = m.positions[q * 4 + 2];
        auto comp = [](cfloat3 p, int i) { return i == 0 ? p.x : (i == 1 ? p.y : p.z); };
        // The face plane, and the two in-plane axes the sweep called u and v.
        const int ua = axis == 0 ? 1 : 0;
        const int va = axis == 2 ? 1 : 2;
        const int a = lround_i(comp(p0, axis)) - (sign > 0 ? 1 : 0);
        const int u0 = lround_i(comp(p0, ua)), v0 = lround_i(comp(p0, va));
        const int u1 = lround_i(comp(p2, ua)), v1 = lround_i(comp(p2, va));
        for (int v = v0; v < v1; ++v)
            for (int u = u0; u < u1; ++u) {
                UnitFace f{axis, sign, a, u, v};
                // A duplicate means two quads cover the same face — an overlap,
                // which is as wrong as a hole.
                REQUIRE(faces.find(f) == faces.end());
                faces[f] = m.colors[q * 4];
            }
    }
    return faces;
}

// Reports WHICH faces disagree, not just that some do. A seam regression here
// is a handful of faces out of tens of thousands, and a bare false leaves the
// next reader bisecting for it when both maps were already in hand.
bool same_surface(const mesh::Mesh& a, const mesh::Mesh& b) {
    std::map<UnitFace, cfloat3> fa = unit_faces(a), fb = unit_faces(b);
    auto describe = [](const char* what, const UnitFace& f) {
        MESSAGE(what << ": axis=" << f.axis << " sign=" << f.sign << " slice=" << f.a
                     << " u=" << f.u << " v=" << f.v);
    };
    int reported = 0;
    bool same = true;
    for (const auto& [f, color] : fa) {
        auto it = fb.find(f);
        if (it == fb.end()) {
            same = false;
            if (reported++ < 8) describe("missing from the second mesh", f);
        } else if (clength(it->second - color) > 1e-6f) {
            same = false;
            if (reported++ < 8) describe("colour differs", f);
        }
    }
    for (const auto& [f, color] : fb) {
        (void)color;
        if (fa.find(f) == fa.end()) {
            same = false;
            if (reported++ < 8) describe("extra in the second mesh", f);
        }
    }
    return same;
}

std::size_t tris(const mesh::Mesh& m) { return m.indices.size() / 3; }

// A grid whose material straddles chunk seams on every axis, in two colours,
// so the merge has something to merge across a boundary and something to keep
// apart.
VoxelGrid seam_grid() {
    VoxelGrid g(1.0f);
    std::uint8_t red = g.palette_add(cf3(1, 0, 0));
    std::uint8_t blue = g.palette_add(cf3(0, 0, 1));
    g.fill_box({-3, -3, -3}, {3, 3, 3}, red);          // across the origin seam
    g.fill_box({kChunkDim - 4, 0, 0}, {kChunkDim + 3, 5, 5}, blue);  // across +x
    g.fill_box({0, kChunkDim - 2, -40}, {2, kChunkDim + 1, -37}, red);
    g.set({kChunkDim * 3 + 7, kChunkDim * 2, kChunkDim - 1}, blue);  // a lone cell
    return g;
}

std::set<VoxelCoord, bool (*)(const VoxelCoord&, const VoxelCoord&)> key_set(
    const std::vector<VoxelCoord>& keys) {
    auto less = +[](const VoxelCoord& a, const VoxelCoord& b) {
        return std::array<std::int32_t, 3>{a.x, a.y, a.z} <
               std::array<std::int32_t, 3>{b.x, b.y, b.z};
    };
    return {keys.begin(), keys.end(), less};
}

bool drained(const std::vector<VoxelCoord>& keys, VoxelCoord want) {
    for (VoxelCoord k : keys)
        if (k == want) return true;
    return false;
}

VoxelCoord key_of(VoxelCoord c) {
    auto fd = [](std::int32_t a) { return a >= 0 ? a / kChunkDim : -((-a + kChunkDim - 1) / kChunkDim); };
    return {fd(c.x), fd(c.y), fd(c.z)};
}

BrushParams sphere(int size) {
    BrushParams p;
    p.size = size;
    p.shape = BrushShape::Sphere;
    return p;
}

}  // namespace

// -- the seam argument -------------------------------------------------------

TEST_CASE("per-chunk meshing describes the same surface as the whole-grid mesh") {
    VoxelGrid g = seam_grid();
    mesh::Mesh whole = g.mesh_greedy();
    mesh::Mesh per = g.mesh_greedy_chunks(g.occupied_chunk_keys());

    CHECK(same_surface(whole, per));
    // Clamping the merge can only split quads, never remove faces.
    CHECK(tris(per) >= tris(whole));
    // And it does split some, or the fixture is not exercising a seam.
    CHECK(tris(per) > tris(whole));
}

TEST_CASE("a quad merged across a chunk seam splits rather than cracks") {
    // One 2x2 column of cells straddling the +x chunk boundary: whole-grid, the
    // +y face is one 2x8 quad; per chunk it is two.
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.fill_box({kChunkDim - 4, 0, 0}, {kChunkDim + 3, 1, 1}, c);

    mesh::Mesh whole = g.mesh_greedy();
    std::vector<VoxelCoord> keys = g.occupied_chunk_keys();
    REQUIRE(keys.size() == 2);
    std::vector<VoxelChunkMeshRange> ranges;
    mesh::Mesh per = g.mesh_greedy_chunks(keys, &ranges);

    CHECK(same_surface(whole, per));
    CHECK(tris(per) == tris(whole) + 8);  // 4 faces along x split into 2 each
    CHECK(ranges.size() == 2);
    CHECK(ranges[0].vertex_count > 0);
    CHECK(ranges[1].vertex_count > 0);
}

TEST_CASE("an unrequested neighbour still hides a face") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({kChunkDim - 1, 0, 0}, c);  // last cell of chunk 0
    g.set({kChunkDim, 0, 0}, c);      // first cell of chunk 1

    mesh::Mesh only_first = g.mesh_greedy_chunks({{0, 0, 0}});
    // Five exposed faces, not six: the neighbour across +x covers one, whether
    // or not the caller asked for that chunk.
    CHECK(tris(only_first) == 10);
    for (const cfloat3& n : only_first.normals) CHECK(n.x <= 0.0f);
}

TEST_CASE("a key holding no chunk meshes to an empty range") {
    VoxelGrid g(1.0f);
    g.set({0, 0, 0}, g.palette_add(cf3(1, 1, 1)));
    std::vector<VoxelChunkMeshRange> ranges;
    mesh::Mesh m = g.mesh_greedy_chunks({{9, 9, 9}, {0, 0, 0}, {-4, 0, 0}}, &ranges);

    REQUIRE(ranges.size() == 3);
    CHECK(ranges[0].vertex_count == 0);
    CHECK(ranges[0].index_count == 0);
    CHECK(ranges[0].vertex_first == 0);
    CHECK(ranges[1].vertex_count == 24);
    CHECK(ranges[2].vertex_count == 0);
    // The empty range sits where its geometry would have begun.
    CHECK(ranges[2].vertex_first == m.positions.size());
    CHECK(tris(m) == 12);
}

TEST_CASE("the ranges partition the mesh with no vertex shared between keys") {
    VoxelGrid g = seam_grid();
    std::vector<VoxelCoord> keys = g.occupied_chunk_keys();
    std::vector<VoxelChunkMeshRange> ranges;
    mesh::Mesh m = g.mesh_greedy_chunks(keys, &ranges);

    REQUIRE(ranges.size() == keys.size());
    std::uint32_t vnext = 0, inext = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        CHECK(ranges[i].key == keys[i]);
        CHECK(ranges[i].vertex_first == vnext);
        CHECK(ranges[i].index_first == inext);
        // Every index in this key's range points into this key's vertices.
        for (std::uint32_t k = 0; k < ranges[i].index_count; ++k) {
            std::uint32_t idx = m.indices[ranges[i].index_first + k];
            CHECK(idx >= ranges[i].vertex_first);
            CHECK(idx < ranges[i].vertex_first + ranges[i].vertex_count);
        }
        vnext += ranges[i].vertex_count;
        inext += ranges[i].index_count;
    }
    CHECK(vnext == m.positions.size());
    CHECK(inext == m.indices.size());
}

TEST_CASE("meshing a subset costs the subset") {
    VoxelGrid g = seam_grid();
    std::vector<VoxelCoord> all = g.occupied_chunk_keys();
    REQUIRE(all.size() >= 3);
    mesh::Mesh one = g.mesh_greedy_chunks({all[0]});
    mesh::Mesh every = g.mesh_greedy_chunks(all);
    CHECK(one.indices.size() < every.indices.size());
    // And the subset's geometry is exactly the slice the full call gave it.
    std::vector<VoxelChunkMeshRange> ranges;
    (void)g.mesh_greedy_chunks(all, &ranges);
    CHECK(ranges[0].index_count == one.indices.size());
}

// -- the dirty set -----------------------------------------------------------

TEST_CASE("a write dirties its chunk and nothing else") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({5, 5, 5}, c);
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == VoxelCoord{0, 0, 0});
}

TEST_CASE("a write on a chunk face dirties the neighbour across it") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    // Seed both chunks so the neighbour exists, then drain.
    g.set({kChunkDim - 1, 0, 0}, c);
    g.set({kChunkDim + 5, 0, 0}, c);
    (void)g.take_dirty_chunks();

    // A write on chunk 0's own +x face changes what chunk 1 shows there.
    g.set({kChunkDim - 1, 1, 0}, c);
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    CHECK(drained(keys, VoxelCoord{0, 0, 0}));
    CHECK(drained(keys, VoxelCoord{1, 0, 0}));

    SUBCASE("and on every other axis too") {
        VoxelGrid h(1.0f);
        std::uint8_t k = h.palette_add(cf3(1, 1, 1));
        h.set({0, 0, 0}, k);
        h.set({-1, 0, 0}, k);
        h.set({0, -1, 0}, k);
        h.set({0, 0, -1}, k);
        (void)h.take_dirty_chunks();
        h.set({0, 0, 0}, 0);  // erase the corner cell: three seams move
        std::vector<VoxelCoord> ks = h.take_dirty_chunks();
        CHECK(drained(ks, VoxelCoord{0, 0, 0}));
        CHECK(drained(ks, VoxelCoord{-1, 0, 0}));
        CHECK(drained(ks, VoxelCoord{0, -1, 0}));
        CHECK(drained(ks, VoxelCoord{0, 0, -1}));
    }
}

TEST_CASE("an absent neighbour chunk is not dirtied") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({kChunkDim - 1, 0, 0}, c);  // on the +x face, with nothing beyond it
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    // A chunk that holds no material emits no quads, so there is nothing there
    // for a host to re-mesh — and any later write into it marks it itself.
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == VoxelCoord{0, 0, 0});
}

TEST_CASE("a chunk erased to empty is still reported") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({kChunkDim * 4 + 5, 5, 5}, c);
    (void)g.take_dirty_chunks();
    CHECK(g.occupied_chunk_keys().size() == 1);

    g.set({kChunkDim * 4 + 5, 5, 5}, 0);
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    // The chunk is gone from the grid, and its key is exactly what a host needs
    // in order to drop the quads it used to draw.
    CHECK(g.occupied_chunk_keys().empty());
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == VoxelCoord{4, 0, 0});
    // ...and meshing that key is an ordinary empty result, not an error.
    std::vector<VoxelChunkMeshRange> ranges;
    mesh::Mesh m = g.mesh_greedy_chunks(keys, &ranges);
    CHECK(m.indices.empty());
    CHECK(ranges[0].index_count == 0);
}

TEST_CASE("an edit that changes nothing dirties nothing") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({1, 1, 1}, c);
    (void)g.take_dirty_chunks();

    g.set({1, 1, 1}, c);            // the value it already holds
    g.set({500, 500, 500}, 0);      // erase in a chunk that does not exist
    g.paint({2, 2, 2}, c);          // paint an empty cell: paints nothing
    CHECK(g.take_dirty_chunks().empty());
}

TEST_CASE("draining empties the set and a later write lands in the next drain") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({1, 1, 1}, c);
    CHECK(g.dirty_chunk_count() == 1);
    CHECK(g.take_dirty_chunks().size() == 1);
    CHECK(g.dirty_chunk_count() == 0);
    CHECK(g.take_dirty_chunks().empty());

    g.set({kChunkDim * 2, 0, 0}, c);
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == VoxelCoord{2, 0, 0});
}

TEST_CASE("a whole-grid mesh neither reads the dirty set nor clears it") {
    VoxelGrid g = seam_grid();
    const std::size_t before = g.dirty_chunk_count();
    REQUIRE(before > 0);
    mesh::Mesh a = g.mesh_greedy();
    CHECK(g.dirty_chunk_count() == before);
    (void)g.take_dirty_chunks();
    mesh::Mesh b = g.mesh_greedy();
    CHECK(a.positions.size() == b.positions.size());
    CHECK(a.indices.size() == b.indices.size());
}

TEST_CASE("the drained order is deterministic") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    for (int i = 9; i >= 0; --i) g.set({i * kChunkDim, (i % 3) * kChunkDim, 0}, c);
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    for (std::size_t i = 1; i < keys.size(); ++i) {
        const bool ordered = std::array<std::int32_t, 3>{keys[i - 1].x, keys[i - 1].y,
                                                         keys[i - 1].z} <
                             std::array<std::int32_t, 3>{keys[i].x, keys[i].y, keys[i].z};
        CHECK(ordered);
    }
}

// Every public verb funnels its writes through one place, so rather than
// asserting a hand-written key list per verb, this diffs the grid across the
// call and demands the drain covers every chunk that actually moved.
TEST_CASE("every mutation verb reports the chunks it changed") {
    auto changed_chunks = [](const VoxelGrid& before, const VoxelGrid& after) {
        std::set<std::array<std::int32_t, 3>> keys;
        for (std::int32_t z = -40; z <= 40; ++z)
            for (std::int32_t y = -40; y <= 40; ++y)
                for (std::int32_t x = -40; x <= 40; ++x) {
                    VoxelCoord c{x, y, z};
                    if (before.get(c) == after.get(c)) continue;
                    VoxelCoord k = key_of(c);
                    keys.insert({k.x, k.y, k.z});
                }
        return keys;
    };

    // `prep` shapes the fixture BEFORE the snapshot, so a verb that needs a
    // pocket or a hole to work on does not get its own setup counted as the
    // change it is supposed to report.
    auto check_prepared = [&](const char* name, auto&& prep, auto&& verb) {
        VoxelGrid g(1.0f);
        std::uint8_t c = g.palette_add(cf3(0.5f, 0.5f, 0.5f));
        // A seeded blob straddling the origin seam, so most verbs have both
        // material and a chunk boundary to work with.
        g.fill_box({-6, -6, -6}, {6, 6, 6}, c);
        g.fill_box({kChunkDim - 3, -2, -2}, {kChunkDim + 2, 2, 2}, c);
        prep(g, c);
        (void)g.take_dirty_chunks();
        VoxelGrid before = g;

        verb(g, c);
        std::vector<VoxelCoord> keys = g.take_dirty_chunks();
        std::set<std::array<std::int32_t, 3>> want = changed_chunks(before, g);
        INFO("verb: " << std::string(name));
        CHECK(!want.empty());  // the fixture must actually exercise the verb
        for (const auto& k : want) CHECK(drained(keys, VoxelCoord{k[0], k[1], k[2]}));
    };
    auto nothing = [](VoxelGrid&, std::uint8_t) {};
    auto check_verb = [&](const char* name, auto&& verb) {
        check_prepared(name, nothing, verb);
    };

    const BrushParams p = sphere(5);
    check_verb("set", [](VoxelGrid& g, std::uint8_t c) { g.set({20, 20, 20}, c); });
    check_verb("erase", [](VoxelGrid& g, std::uint8_t) { g.erase({0, 0, 0}); });
    check_verb("paint", [](VoxelGrid& g, std::uint8_t) {
        g.paint({0, 0, 0}, g.palette_add(cf3(1, 0, 0)));
    });
    check_verb("set_brush", [](VoxelGrid& g, std::uint8_t c) { g.set_brush({20, 0, 0}, 5, c); });
    check_verb("erase_brush", [](VoxelGrid& g, std::uint8_t) { g.erase_brush({0, 0, 0}, 5); });
    check_verb("paint_brush", [](VoxelGrid& g, std::uint8_t) {
        g.paint_brush({0, 0, 0}, 5, g.palette_add(cf3(1, 0, 0)));
    });
    check_verb("fill_box", [](VoxelGrid& g, std::uint8_t c) {
        g.fill_box({30, 30, 30}, {34, 34, 34}, c);
    });
    check_verb("fill_line", [](VoxelGrid& g, std::uint8_t c) {
        g.fill_line({-30, 0, 0}, {30, 0, 0}, c);
    });
    // The spec scenario names stroke application among the verbs that report
    // what they touched, so it is gated here rather than left to the reader to
    // infer from apply_to_grid funnelling through set_brush.
    check_verb("brush::apply_to_grid", [](VoxelGrid& g, std::uint8_t c) {
        std::vector<brush::Stamp> stamps;
        for (int i = 0; i < 4; ++i) {
            brush::Stamp s;
            s.position = cf3(static_cast<float>(kChunkDim - 2 + i) * g.voxel_size(), 0.0f, 0.0f);
            s.radius = 3.0f * g.voxel_size();
            stamps.push_back(s);
        }
        brush::apply_to_grid(g, stamps, c);
    });
    check_verb("set_mirrored", [](VoxelGrid& g, std::uint8_t c) {
        g.set_mirrored({20, 20, 20}, c, voxel::kVoxMirrorX | voxel::kVoxMirrorY);
    });
    check_verb("paint_mirrored", [](VoxelGrid& g, std::uint8_t) {
        g.paint_mirrored({0, 0, 0}, g.palette_add(cf3(1, 0, 0)),
                         voxel::kVoxMirrorX | voxel::kVoxMirrorY);
    });
    check_verb("sculpt_inflate", [&](VoxelGrid& g, std::uint8_t) {
        g.sculpt_inflate({6, 0, 0}, p, 1);
    });
    check_verb("sculpt_smooth", [&](VoxelGrid& g, std::uint8_t) {
        // A corner is not a fixed point of the majority filter, unlike a face.
        g.sculpt_smooth({6, 6, 6}, p);
    });
    check_verb("sculpt_flatten", [&](VoxelGrid& g, std::uint8_t) {
        g.sculpt_flatten({6, 0, 0}, p, cf3(1, 0, 0), -1.0f);
    });
    check_verb("sculpt_pinch", [&](VoxelGrid& g, std::uint8_t) { g.sculpt_pinch({6, 0, 0}, p); });
    check_verb("sculpt_magnify", [&](VoxelGrid& g, std::uint8_t) {
        g.sculpt_magnify({6, 0, 0}, p);
    });
    check_verb("sculpt_scrape", [&](VoxelGrid& g, std::uint8_t) {
        g.sculpt_scrape({6, 6, 6}, p, cf3(1, 1, 1), -1.0f);
    });
    check_verb("sculpt_smudge", [&](VoxelGrid& g, std::uint8_t) {
        g.sculpt_smudge({6, 0, 0}, p, cf3(2, 0, 0));
    });
    // Grabbing at the surface: an interior grab moves solid into solid and is
    // legitimately a no-op.
    check_verb("sculpt_grab", [&](VoxelGrid& g, std::uint8_t) {
        g.sculpt_grab({6, 0, 0}, p, cf3(3, 0, 0));
    });
    check_prepared(
        "sculpt_fill_cavities",
        [](VoxelGrid& g, std::uint8_t) { g.set({0, 0, 0}, 0); },  // a one-cell pocket
        [&](VoxelGrid& g, std::uint8_t) { g.sculpt_fill_cavities({0, 0, 0}, p, 1); });
    check_verb("sculpt_carve_alpha", [&](VoxelGrid& g, std::uint8_t) {
        std::vector<float> alpha(16 * 16, 1.0f);
        CHECK(g.sculpt_carve_alpha({6, 0, 0}, p, alpha.data(), 16, 16, cf3(1, 0, 0), 0));
    });
    check_prepared(
        "repair_close_holes",
        [](VoxelGrid& g, std::uint8_t) { g.set({0, 0, 0}, 0); },  // pierce the shell
        [](VoxelGrid& g, std::uint8_t) { g.repair_close_holes(1); });
    check_prepared(
        "repair_fill_voids", [](VoxelGrid& g, std::uint8_t) { g.set({0, 0, 0}, 0); },
        [](VoxelGrid& g, std::uint8_t) { g.repair_fill_voids(); });
}

TEST_CASE("rasterizing and deserializing report every chunk they wrote") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = clay_test::item(scene::Prim::sphere(1.6f), cf3(0, 0, 0));
    n.color = cf3(0.8f, 0.2f, 0.1f);
    l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    VoxelGrid g(0.05f);
    g.rasterize_tape(tape, math::Aabb{cf3(-1.8f, -1.8f, -1.8f), cf3(1.8f, 1.8f, 1.8f)});
    std::vector<VoxelCoord> keys = g.take_dirty_chunks();
    CHECK(key_set(keys) == key_set(g.occupied_chunk_keys()));

    // A grid read back from a file has drawn nothing yet, so it reports
    // everything: the first full display and an incremental one are one path.
    std::vector<std::uint8_t> bytes = g.serialize();
    auto back = VoxelGrid::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(key_set(back->take_dirty_chunks()) == key_set(back->occupied_chunk_keys()));
}

TEST_CASE("the dirty set is per level and propagation feeds every level it writes") {
    VoxelGrid g(1.0f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.set({4, 4, 4}, c);
    REQUIRE(g.add_level() == 1);
    // Subdividing wrote the fine level, so it reports what it wrote.
    CHECK(g.dirty_chunk_count(0) > 0);
    CHECK(g.dirty_chunk_count(1) > 0);
    (void)g.take_dirty_chunks(0);
    (void)g.take_dirty_chunks(1);

    // An edit on the coarse level propagates up, so BOTH levels report.
    REQUIRE(g.set_active_level(0));
    g.set({20, 20, 20}, c);
    CHECK(g.take_dirty_chunks(0).size() == 1);
    CHECK(g.take_dirty_chunks(1).size() >= 1);
    // ...and each drain is its own.
    CHECK(g.dirty_chunk_count(0) == 0);
    CHECK(g.dirty_chunk_count(1) == 0);
}

// -- incremental against whole -----------------------------------------------

TEST_CASE("a patched incremental mesh equals a from-scratch whole-grid mesh") {
    VoxelGrid g = seam_grid();

    // A host's per-chunk store: key -> the geometry that key contributes.
    std::map<std::array<std::int32_t, 3>, mesh::Mesh> shards;
    auto patch = [&](const std::vector<VoxelCoord>& keys) {
        std::vector<VoxelChunkMeshRange> ranges;
        mesh::Mesh m = g.mesh_greedy_chunks(keys, &ranges);
        for (const VoxelChunkMeshRange& r : ranges) {
            mesh::Mesh shard;
            for (std::uint32_t i = 0; i < r.vertex_count; ++i) {
                shard.positions.push_back(m.positions[r.vertex_first + i]);
                shard.normals.push_back(m.normals[r.vertex_first + i]);
                shard.colors.push_back(m.colors[r.vertex_first + i]);
            }
            for (std::uint32_t i = 0; i < r.index_count; ++i)
                shard.indices.push_back(m.indices[r.index_first + i] - r.vertex_first);
            std::array<std::int32_t, 3> k{r.key.x, r.key.y, r.key.z};
            if (shard.indices.empty())
                shards.erase(k);  // the chunk went: drop its quads
            else
                shards[k] = std::move(shard);
        }
    };
    auto assembled = [&]() {
        mesh::Mesh out;
        for (const auto& [key, shard] : shards) {
            std::uint32_t base = static_cast<std::uint32_t>(out.positions.size());
            out.positions.insert(out.positions.end(), shard.positions.begin(),
                                 shard.positions.end());
            out.normals.insert(out.normals.end(), shard.normals.begin(), shard.normals.end());
            out.colors.insert(out.colors.end(), shard.colors.begin(), shard.colors.end());
            for (std::uint32_t i : shard.indices) out.indices.push_back(base + i);
        }
        return out;
    };

    patch(g.take_dirty_chunks());  // the first full display
    CHECK(same_surface(g.mesh_greedy(), assembled()));

    // Now a stroke: a deposit across a seam, an erase that empties a chunk, and
    // a sculpt. Each display re-meshes only what the drain reported.
    std::uint8_t green = g.palette_add(cf3(0, 1, 0));
    g.set_brush({kChunkDim - 1, 0, 0}, sphere(7), green);
    patch(g.take_dirty_chunks());
    CHECK(same_surface(g.mesh_greedy(), assembled()));

    g.set({kChunkDim * 3 + 7, kChunkDim * 2, kChunkDim - 1}, 0);  // empties a chunk
    patch(g.take_dirty_chunks());
    CHECK(same_surface(g.mesh_greedy(), assembled()));

    g.sculpt_smooth({3, 3, 3}, sphere(9));
    patch(g.take_dirty_chunks());
    CHECK(same_surface(g.mesh_greedy(), assembled()));

    g.erase_brush({0, 0, 0}, sphere(11));
    patch(g.take_dirty_chunks());
    CHECK(same_surface(g.mesh_greedy(), assembled()));
}
