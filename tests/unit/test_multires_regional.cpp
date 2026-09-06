// REFINING ONE REGION of a hierarchy (mesh-multires spec,
// refine-one-region-of-a-hierarchy).
//
// The claim the feature makes is narrow and numeric, and every gate here is one
// half of it:
//
//   A REGIONAL LEVEL IS THE DENSE LEVEL, WHERE IT EXISTS. Every vertex a
//   regional level stores holds the value the uniformly refined hierarchy would
//   have held at the same point on the surface, bit for bit. That is not a
//   tolerance and it is not a claim about smoothness: the stencils are
//   evaluated against the same parent neighbourhood, so the arithmetic is the
//   same arithmetic. It is also the whole watertightness argument — a fine
//   patch's boundary is the exact subdivision of the coarse edge it meets
//   because it is literally the same computation.
//
//   AND IT COSTS THE REFINED AREA. Topology, the evaluated buffers and the
//   chunk index all follow the faces a level actually holds, which is what
//   makes "level 5 on the nose" affordable when "level 5 everywhere" is not.
//
// The cage throughout is a flat grid, because a patch id is then a cell of a
// grid and a test can name a region by arithmetic rather than by inspection.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "clay/mesh/multires.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MultiresError;
using mesh::MultiresSurface;

namespace {

// An n x n grid of quads. Patch `z * n + x` is the cell at (x, z), which is the
// order `base_topology_from_mesh` emits them in.
Mesh grid_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x),
                                      0.15f * static_cast<float>((x * 7 + z * 3) % 5),
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

MultiresSurface build(const Mesh& cage) {
    MultiresError err = MultiresError::None;
    std::optional<MultiresSurface> s = MultiresSurface::from_mesh(cage, {}, &err);
    REQUIRE(s.has_value());
    return std::move(*s);
}

// One patch's faces at a level, as their CORNER POSITIONS in face order.
//
// Compared this way rather than by vertex id, and that is the point rather than
// a convenience: a regional hierarchy numbers its vertices compactly, so the
// same point on the surface has a different id in the two hierarchies. What is
// the same in both is the FACE ORDER — a patch's faces are a contiguous run
// emitted parent-face by parent-face — so walking corners side by side compares
// the same points.
std::vector<cfloat3> patch_corners(MultiresSurface& s, std::uint32_t level, std::uint32_t patch) {
    const mesh::LevelTopology& t = s.topology_at(level);
    const std::vector<cfloat3>& p = s.positions_at(level);
    std::vector<cfloat3> out;
    for (std::uint32_t f = 0; f < t.face_count; ++f) {
        if (t.patch_of(f) != patch) continue;
        std::uint32_t arity = 0;
        const std::uint32_t* corners = t.face(f, &arity);
        for (std::uint32_t i = 0; i < arity; ++i) out.push_back(p[corners[i]]);
    }
    return out;
}

bool same_bits(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// The patches of a d x d block whose top-left cell is (x0, z0), on an n x n cage.
std::vector<std::uint32_t> block_patches(int n, int x0, int z0, int d) {
    std::vector<std::uint32_t> out;
    for (int z = z0; z < z0 + d; ++z)
        for (int x = x0; x < x0 + d; ++x)
            out.push_back(static_cast<std::uint32_t>(z * n + x));
    return out;
}

}  // namespace

TEST_CASE("regional: naming every patch IS the uniform level") {
    const Mesh cage = grid_quads(4, 1.0f);
    MultiresSurface dense = build(cage);
    MultiresSurface all = build(cage);

    std::vector<std::uint32_t> every;
    for (std::uint32_t p = 0; p < 16; ++p) every.push_back(p);

    for (int l = 0; l < 3; ++l) {
        REQUIRE(dense.add_level());
        REQUIRE(all.add_level_for_patches(every));
    }
    // Not "equal to": the SAME LEVEL. A request naming everything takes the
    // dense path, so there is no second implementation of a uniform level to
    // drift from this one.
    CHECK(all.uniform_depth());
    CHECK(all.topology_at(3).dense());
    CHECK(dense.encode() == all.encode());
    CHECK(same_bits(patch_corners(dense, 3, 5), patch_corners(all, 3, 5)));
}

TEST_CASE("regional: a refined patch holds the dense hierarchy's own numbers") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface dense = build(cage);
    MultiresSurface part = build(cage);
    for (int l = 0; l < 3; ++l) REQUIRE(dense.add_level());
    // The middle 2x2 to level 3, which grades the levels below it out to the
    // rings those stencils need.
    REQUIRE(part.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));
    REQUIRE(part.max_level() == 3);
    CHECK_FALSE(part.uniform_depth());

    // Every patch the regional hierarchy kept at level 3 carries the dense
    // hierarchy's own bits. BIT-IDENTICAL, not close: the two evaluate the same
    // stencils over the same parent neighbourhood.
    std::uint32_t compared = 0;
    for (std::uint32_t p = 0; p < 36; ++p) {
        if (!part.patch_resident(3, p)) continue;
        INFO("patch " << p);
        CHECK(same_bits(patch_corners(dense, 3, p), patch_corners(part, 3, p)));
        ++compared;
    }
    CHECK(compared == 4);

    // ...and so does every patch at the coarser level it was graded to, which
    // is the transition itself: the fine side's boundary is the exact
    // subdivision of the edge the coarse side holds.
    for (std::uint32_t p = 0; p < 36; ++p) {
        if (!part.patch_resident(2, p)) continue;
        INFO("patch " << p << " at level 2");
        CHECK(same_bits(patch_corners(dense, 2, p), patch_corners(part, 2, p)));
    }
}

TEST_CASE("regional: depth is a property of a patch") {
    // Ten cells across, so the two rings the grading adds do not swallow the
    // cage -- on a six-cell grid they reach every patch and the interesting
    // case disappears.
    const Mesh cage = grid_quads(10, 1.0f);
    MultiresSurface s = build(cage);
    REQUIRE(s.refine_patches_to_level(block_patches(10, 4, 4, 2), 3));

    // The named block reaches 3; the ring around it 2; the ring around THAT 1;
    // and the rest of the cage is the cage. That IS the grading, read back.
    for (std::uint32_t p : block_patches(10, 4, 4, 2)) CHECK(s.patch_max_level(p) == 3);
    CHECK(s.patch_max_level(10 * 3 + 3) == 2);  // a corner of the first ring
    CHECK(s.patch_max_level(10 * 2 + 2) == 1);  // the second ring
    CHECK(s.patch_max_level(0) == 0);           // never refined at all
    CHECK(s.effective_level(0, 3) == 0);
    CHECK(s.effective_level(10 * 4 + 4, 3) == 3);
    CHECK(s.effective_level(10 * 4 + 4, 1) == 1);

    // Asking again for a depth the hierarchy already has is a no-op rather
    // than four more levels, and asking for a SHALLOWER one is too — this call
    // reaches a depth, it does not add one.
    const std::vector<std::uint8_t> before = s.encode();
    REQUIRE(s.refine_patches_to_level(block_patches(10, 4, 4, 2), 3));
    REQUIRE(s.refine_patches_to_level(block_patches(10, 4, 4, 2), 1));
    CHECK(s.max_level() == 3);
    CHECK(s.encode() == before);
}

TEST_CASE("regional: refining with no detail does not move the surface") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface flat = build(cage);
    MultiresSurface refined = build(cage);
    REQUIRE(flat.refine_patches_to_level(block_patches(6, 2, 2, 2), 2));
    REQUIRE(refined.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));

    // A level added over a patch changes nothing anybody was looking at until
    // something is authored into it. Level 2 is where both hierarchies still
    // agree about what exists.
    for (std::uint32_t p = 0; p < 36; ++p) {
        if (!flat.patch_resident(2, p)) continue;
        INFO("patch " << p);
        CHECK(same_bits(patch_corners(flat, 2, p), patch_corners(refined, 2, p)));
    }
    // ...and the level it added costs nothing while nobody has authored into
    // it, which is `DetailField`'s own sparsity restated for a regional level.
    // ...against the twelve bytes a vertex a dense field would have cost.
    CHECK(refined.detail_at(3).bytes() < refined.topology_at(3).vertex_count);
}

TEST_CASE("regional: the same request twice is the same hierarchy") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface a = build(cage);
    MultiresSurface b = build(cage);
    // The second request names the same patches in the opposite order, because
    // a set is a set: an implementation that grew its rings in arrival order
    // would produce a different surface here.
    std::vector<std::uint32_t> forward = block_patches(6, 1, 3, 3);
    std::vector<std::uint32_t> backward(forward.rbegin(), forward.rend());
    REQUIRE(a.refine_patches_to_level(forward, 3));
    REQUIRE(b.refine_patches_to_level(backward, 3));
    CHECK(a.encode() == b.encode());
}

TEST_CASE("regional: a patch whose neighbourhood is missing is refused") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface s = build(cage);
    MultiresError err = MultiresError::None;

    CHECK_FALSE(s.add_level_for_patches({}, &err));
    CHECK(err == MultiresError::NoPatchesRequested);

    // Level 1 over the middle 2x2 only. Level 2 over the same block then asks
    // for stencils whose parent ring does not exist -- Catmull-Clark's BORDER
    // rule at an edge that is not a border, which is a crack, so it is refused
    // rather than approximated.
    REQUIRE(s.add_level_for_patches(block_patches(6, 2, 2, 2), &err));
    CHECK_FALSE(s.add_level_for_patches(block_patches(6, 2, 2, 2), &err));
    CHECK(err == MultiresError::PatchNotRefinable);
    // ...and a patch that was never refined at level 1 is refused for the same
    // reason, one step earlier.
    CHECK_FALSE(s.add_level_for_patches({0u}, &err));
    CHECK(err == MultiresError::PatchNotRefinable);
    CHECK(s.max_level() == 1);
}

TEST_CASE("regional: memory follows the refined area") {
    // Big enough that the ratio is about the geometry rather than about
    // per-level fixed costs: 16x16 patches, four of them refined deep.
    const Mesh cage = grid_quads(16, 1.0f);
    MultiresSurface dense = build(cage);
    MultiresSurface part = build(cage);
    for (int l = 0; l < 4; ++l) REQUIRE(dense.add_level());
    REQUIRE(part.refine_patches_to_level(block_patches(16, 7, 7, 2), 4));

    // Forced resident on both sides, so this compares two hierarchies in the
    // same state rather than one that has been evaluated against one that has
    // not.
    dense.positions_at(4);
    part.positions_at(4);
    // The chunk table is built on demand, so a comparison that never asked for
    // one would be comparing two levels that have none.
    dense.level_chunks(4);
    part.level_chunks(4);

    const mesh::MultiresMemory d = dense.memory(), r = part.memory();
    INFO("dense  topology " << d.topology << " evaluated " << d.evaluated << " chunks "
                            << d.chunk_index << " total " << d.total);
    INFO("region topology " << r.topology << " evaluated " << r.evaluated << " chunks "
                            << r.chunk_index << " total " << r.total);

    // THE CLAIM, and it is a ratio rather than a byte count so it survives an
    // allocator that rounds differently. The regional hierarchy refines 4 of
    // 256 patches to level 4 and grades three rings below that, so its deep
    // levels hold a small fraction of the dense ones.
    CHECK(r.topology * 4 < d.topology);
    CHECK(r.evaluated * 4 < d.evaluated);
    CHECK(r.chunk_index * 4 < d.chunk_index);
    CHECK(r.total * 4 < d.total);

    // A level that is not refined has no faces AT ALL, which is the statement
    // "no storage and read at its own level" in the form a test can check.
    CHECK(part.topology_at(4).face_count == 4u * 256u);
    CHECK(dense.topology_at(4).face_count == 256u * 256u);
}

TEST_CASE("regional: the work of an evaluation follows the refined area too") {
    // COUNTED, not timed. The claim is that a level's evaluation touches the
    // vertices the level HAS, and a wall clock would measure the machine as
    // much as the change; `MultiresEvalStats` says exactly what was done.
    const Mesh cage = grid_quads(16, 1.0f);
    MultiresSurface dense = build(cage);
    MultiresSurface part = build(cage);
    for (int l = 0; l < 4; ++l) REQUIRE(dense.add_level());
    REQUIRE(part.refine_patches_to_level(block_patches(16, 7, 7, 2), 4));

    dense.reset_eval_stats();
    part.reset_eval_stats();
    dense.positions_at(4);
    part.positions_at(4);

    const std::uint64_t d = dense.eval_stats().vertices_evaluated;
    const std::uint64_t r = part.eval_stats().vertices_evaluated;
    INFO("dense evaluated " << d << " vertices, regional " << r);
    CHECK(r * 4 < d);
}

TEST_CASE("regional: a mixed-depth hierarchy survives a round trip") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface s = build(cage);
    REQUIRE(s.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));

    // With detail on it, so the round trip is carrying coefficients against a
    // level whose vertex count is the REGIONAL one.
    mesh::DetailField& detail = s.detail_mutable(3);
    for (std::uint32_t v = 0; v < s.topology_at(3).vertex_count; v += 3)
        detail.set(v, LocalDetail{0.0f, 0.0f, 0.02f});

    const std::vector<std::uint8_t> bytes = s.encode();
    MultiresSurface back;
    REQUIRE(MultiresSurface::decode(bytes.data(), bytes.size(), &back));

    CHECK(back.max_level() == 3);
    CHECK_FALSE(back.uniform_depth());
    CHECK(back.detail_checksum() == s.detail_checksum());
    for (std::uint32_t p = 0; p < 36; ++p) {
        INFO("patch " << p);
        CHECK(back.patch_max_level(p) == s.patch_max_level(p));
    }
    CHECK(same_bits(patch_corners(s, 3, 6 * 2 + 2), patch_corners(back, 3, 6 * 2 + 2)));
    // Re-encoding the decoded surface reproduces the stream, which is what
    // makes the patch sets a faithful record of the build rather than a hint.
    CHECK(back.encode() == bytes);
}

TEST_CASE("regional: detail authored on a refined patch stays local to it") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface plain = build(cage);
    MultiresSurface bumped = build(cage);
    REQUIRE(plain.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));
    REQUIRE(bumped.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));

    // Written before the level is first evaluated, which is the state a freshly
    // added level is in: everything pending, nothing yet built.
    mesh::DetailField& detail = bumped.detail_mutable(3);
    for (std::uint32_t v = 0; v < bumped.topology_at(3).vertex_count; ++v)
        detail.set(v, LocalDetail{0.0f, 0.0f, 0.05f});

    const std::uint32_t near_patch = 6u * 2u + 2u, far_patch = 6u * 5u + 5u;
    CHECK_FALSE(same_bits(patch_corners(plain, 3, near_patch),
                          patch_corners(bumped, 3, near_patch)));
    // The form beneath it is untouched: a coefficient at level 3 displaces the
    // level it is stored on, and the cage two levels down never hears about it.
    CHECK(same_bits(patch_corners(plain, 1, far_patch), patch_corners(bumped, 1, far_patch)));
    CHECK(same_bits(patch_corners(plain, 1, near_patch), patch_corners(bumped, 1, near_patch)));
}

// -- the complete neighbourhood across a depth boundary -----------------------
//
// A regional level's own connectivity ends at the region rim, so every walk
// built on it sees an open border where the surface in fact continues one level
// down. `MultiresSurface::cross_level_at` is the faces that walk is missing, and
// these are the gates on it: at a boundary vertex the complete answer is the
// UNIFORM hierarchy's answer, as a count and as a set.

namespace {

// The complete incident-face set of one level vertex, as face CENTROIDS.
//
// Centroids rather than face ids, and for the same reason `patch_corners`
// compares positions: a regional level numbers its vertices compactly, so the
// same point on the surface has a different id in the two hierarchies. A face
// is the same four points in the same corner order in both, so its centroid is
// the same bits.
std::vector<cfloat3> incident_faces(MultiresSurface& s, std::uint32_t level, std::uint32_t v,
                                    bool complete) {
    const mesh::CrossLevelNeighborhood& x = s.cross_level_at(level);
    const mesh::LevelTopology& t = s.topology_at(level);
    const mesh::LevelConnectivity& conn = s.connectivity_at(level);
    const std::vector<cfloat3>& p = s.positions_at(level);
    std::vector<cfloat3> out;
    std::size_t n = 0;
    const std::uint32_t* faces = conn.faces_of(v, &n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t arity = 0;
        const std::uint32_t* c = t.face(faces[i], &arity);
        cfloat3 sum = p[c[0]];
        for (std::uint32_t k = 1; k < arity; ++k) sum = sum + p[c[k]];
        out.push_back(sum / static_cast<float>(arity));
    }
    if (complete && !x.empty()) {
        std::size_t m = 0;
        const std::uint32_t* derived = x.faces_of(v, &m);
        for (std::size_t i = 0; i < m; ++i) {
            const std::uint32_t* c = x.face_corners(derived[i]);
            cfloat3 sum = x.position(p, c[0]);
            for (int k = 1; k < 4; ++k) sum = sum + x.position(p, c[k]);
            out.push_back(sum / 4.0f);
        }
    }
    std::sort(out.begin(), out.end(), [](const cfloat3& a, const cfloat3& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    return out;
}

// Two face sets, bit for bit. `cfloat3` carries no equality operator, and a
// tolerance here would be the tolerance welding this feature refuses.
bool same_faces(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// Which dense vertex holds each of these positions. Exact keys, because the two
// hierarchies agree bit for bit wherever both store a point — which is the
// claim the first gate in this file makes.
std::map<std::array<float, 3>, std::uint32_t> vertex_by_position(MultiresSurface& s,
                                                                 std::uint32_t level) {
    std::map<std::array<float, 3>, std::uint32_t> out;
    const std::vector<cfloat3>& p = s.positions_at(level);
    for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(p.size()); ++v)
        out[{p[v].x, p[v].y, p[v].z}] = v;
    return out;
}

}  // namespace

TEST_CASE("regional: a boundary vertex has the neighbourhood a uniform hierarchy gives it") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface dense = build(cage);
    MultiresSurface part = build(cage);
    for (int l = 0; l < 3; ++l) REQUIRE(dense.add_level());
    REQUIRE(part.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));

    const std::map<std::array<float, 3>, std::uint32_t> dense_of = vertex_by_position(dense, 3);
    const std::uint32_t count = part.topology_at(3).vertex_count;
    CHECK(count == 289);

    std::size_t matched = 0, short_ring = 0, complete_differs = 0;
    for (std::uint32_t v = 0; v < count; ++v) {
        const cfloat3 at = part.positions_at(3)[v];
        const auto it = dense_of.find({at.x, at.y, at.z});
        REQUIRE(it != dense_of.end());
        ++matched;
        const std::vector<cfloat3> reference = incident_faces(dense, 3, it->second, true);
        if (!same_faces(incident_faces(part, 3, v, false), reference)) ++short_ring;
        if (!same_faces(incident_faces(part, 3, v, true), reference)) ++complete_differs;
    }
    CHECK(matched == 289);
    // WHAT THE LEVEL'S OWN CONNECTIVITY REPORTS, and the size of the problem:
    // 64 of 289 — 22% — because a small refined region is proportionally more
    // boundary than a large one. A gate built on a big region under-reports.
    CHECK(short_ring == 64);
    // ...and what the complete neighbourhood reports, which is the uniform
    // hierarchy's own face set, as a count and as a set.
    CHECK(complete_differs == 0);
}

TEST_CASE("regional: the complete neighbourhood is derived and rebuilds identically") {
    const Mesh cage = grid_quads(6, 1.0f);
    MultiresSurface s = build(cage);
    REQUIRE(s.refine_patches_to_level(block_patches(6, 2, 2, 2), 3));

    const mesh::CrossLevelNeighborhood before = s.cross_level_at(3);
    CHECK_FALSE(before.empty());
    CHECK(before.vertex_count == 289);
    const std::uint64_t generation = s.cache_generation();

    s.drop_all_caches();
    // Released, so a host holding a pointer into the cache rebinds rather than
    // reading storage that is gone. The transition set is in the cache with
    // everything else derived, so it inherits that for free.
    CHECK(s.cache_generation() != generation);

    const mesh::CrossLevelNeighborhood after = s.cross_level_at(3);
    CHECK(after.corners == before.corners);
    CHECK(after.dense_face == before.dense_face);
    CHECK(after.face_patch == before.face_patch);
    CHECK(after.outside_layout == before.outside_layout);
    CHECK(after.face_offsets == before.face_offsets);
    CHECK(after.faces == before.faces);
    CHECK(after.ring_offsets == before.ring_offsets);
    CHECK(after.ring == before.ring);
    REQUIRE(after.outside_positions.size() == before.outside_positions.size());
    std::size_t moved = 0;
    for (std::size_t i = 0; i < after.outside_positions.size(); ++i)
        if (after.outside_positions[i].x != before.outside_positions[i].x ||
            after.outside_positions[i].y != before.outside_positions[i].y ||
            after.outside_positions[i].z != before.outside_positions[i].z)
            ++moved;
    CHECK(moved == 0);
}

TEST_CASE("regional: a uniform level has nothing outside it") {
    const Mesh cage = grid_quads(4, 1.0f);
    MultiresSurface s = build(cage);
    for (int l = 0; l < 3; ++l) REQUIRE(s.add_level());
    // THE PARITY ARGUMENT, as a construction rather than a comparison: away
    // from a depth boundary there is nothing to add, so every reader takes
    // exactly the path it took before this change and cannot produce a
    // different number.
    for (std::uint32_t l = 0; l <= 3; ++l) {
        INFO("level " << l);
        CHECK(s.cross_level_at(l).empty());
        CHECK(s.cross_level_at(l).face_count() == 0);
    }
}

// -- the mixed-depth export ---------------------------------------------------
//
// The residual `refine-one-region-of-a-hierarchy` left as task 2.3, and the
// claim it makes is one sentence: A SURFACE OF SEVERAL DEPTHS EXPORTS AS ONE
// CLOSED MESH, and it is closed because the two sides of a depth boundary are
// the SAME INDEX rather than two indices that happen to be near each other.
//
// So the gates below are about what a transition must GUARANTEE and not about
// what one hierarchy happens to produce. Every edge of the emitted mesh is
// shared by exactly two triangles; the same request twice is the same bytes;
// refining somewhere else leaves this place alone. The cage is a TORUS —
// closed, so "0 boundary edges" is a statement about the export rather than
// about the cage's own rim, and the control that the loop a host is told to
// write today does NOT close is the same count on the same surface.

namespace {

// A closed torus of nu x nv quads. No boundary, every base patch a grid cell,
// and `u * nv + v` is the patch at (u, v) — the order `base_topology_from_mesh`
// emits them in, as for `grid_quads`.
Mesh closed_torus(int nu, int nv) {
    Mesh m;
    const float kTwoPi = 6.2831853f;
    for (int u = 0; u < nu; ++u)
        for (int v = 0; v < nv; ++v) {
            const float a = kTwoPi * static_cast<float>(u) / static_cast<float>(nu);
            const float b = kTwoPi * static_cast<float>(v) / static_cast<float>(nv);
            const float radius = 1.0f + 0.4f * std::cos(b);
            m.positions.push_back(cf3(radius * std::cos(a), 0.4f * std::sin(b),
                                      radius * std::sin(a)));
            m.normals.push_back(
                cf3(std::cos(b) * std::cos(a), std::sin(b), std::cos(b) * std::sin(a)));
        }
    const auto at = [&](int u, int v) {
        return static_cast<std::uint32_t>(((u % nu + nu) % nu) * nv + ((v % nv + nv) % nv));
    };
    for (int u = 0; u < nu; ++u)
        for (int v = 0; v < nv; ++v) {
            const std::uint32_t a = at(u, v), b = at(u + 1, v), c = at(u + 1, v + 1),
                                d = at(u, v + 1);
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

std::vector<std::uint32_t> torus_block(int n, int u0, int u1, int v0, int v1) {
    std::vector<std::uint32_t> out;
    for (int u = u0; u <= u1; ++u)
        for (int v = v0; v <= v1; ++v) out.push_back(static_cast<std::uint32_t>(u * n + v));
    return out;
}

// Edges of a triangle list that are NOT shared by exactly two triangles. Zero
// on a closed surface, and the one number that says whether a mixed-depth
// export is watertight: a T-junction and a corner gap both show up here and
// nothing else does.
std::size_t open_edges(const std::vector<std::uint32_t>& indices) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> shared;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = indices[t + k], b = indices[t + (k + 1) % 3];
            if (a > b) std::swap(a, b);
            shared[{a, b}]++;
        }
    std::size_t open = 0;
    for (const auto& edge : shared)
        if (edge.second != 2) ++open;
    return open;
}

// A host's per-patch loop, assembled the way a host must assemble it: one draw
// per patch, welded at EXACTLY zero tolerance. Nothing here is allowed an
// epsilon — the whole claim is that the two sides are the same bits.
struct Assembled {
    std::size_t vertices = 0;
    std::vector<std::uint32_t> indices;
};

Assembled weld_exact(const std::vector<cfloat3>& positions,
                     const std::vector<std::uint32_t>& indices) {
    std::map<std::array<float, 3>, std::uint32_t> at;
    std::vector<std::uint32_t> local(positions.size(), 0u);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const std::array<float, 3> key{positions[i].x, positions[i].y, positions[i].z};
        const auto found = at.emplace(key, static_cast<std::uint32_t>(at.size()));
        local[i] = found.first->second;
    }
    Assembled out;
    out.vertices = at.size();
    for (std::uint32_t i : indices) out.indices.push_back(local[i]);
    return out;
}

std::vector<cfloat3> block_positions(MultiresSurface& s, const MultiresSurface::Block& b) {
    std::vector<cfloat3> out;
    for (std::size_t k = 0; k < b.vertices.size(); ++k) {
        const std::uint32_t level = b.vertex_levels.empty() ? b.level : b.vertex_levels[k];
        out.push_back(s.positions_at(level)[b.vertices[k]]);
    }
    return out;
}

// Every patch's block, laid end to end and welded by position. `mixed` chooses
// between the loop a host is told to write today and the one this change adds.
Assembled assemble_patches(MultiresSurface& s, std::uint32_t display, bool mixed) {
    std::vector<cfloat3> positions;
    std::vector<std::uint32_t> indices;
    MultiresSurface::Block b;
    const std::uint32_t patches = s.topology_at(0).face_count;
    for (std::uint32_t p = 0; p < patches; ++p) {
        const bool ok = mixed ? s.build_mixed_block(display, p, &b)
                              : s.build_block(s.effective_level(p, display), p, &b);
        if (!ok) continue;
        const std::uint32_t base = static_cast<std::uint32_t>(positions.size());
        const std::vector<cfloat3> local = block_positions(s, b);
        positions.insert(positions.end(), local.begin(), local.end());
        for (std::uint32_t i : b.indices) indices.push_back(base + i);
    }
    return weld_exact(positions, indices);
}

bool same_floats(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

bool same_mesh(const Mesh& a, const Mesh& b) {
    return a.indices == b.indices && a.quads == b.quads && same_floats(a.positions, b.positions) &&
           same_floats(a.normals, b.normals);
}

// How many faces this patch has at the level it is emitted from. A quad face
// emits two triangles; a face carrying a split edge emits three, so the
// triangle count against twice this number is what says whether a T-junction
// was bridged — without a new accessor for something the export does not store.
std::uint32_t patch_face_count(MultiresSurface& s, std::uint32_t level, std::uint32_t patch) {
    const mesh::LevelTopology& t = s.topology_at(level);
    std::uint32_t faces = 0;
    for (std::uint32_t f = 0; f < t.face_count; ++f)
        if (t.patch_of(f) == patch) ++faces;
    return faces;
}

}  // namespace

TEST_CASE("regional export: a mixed-depth export closes what the per-patch loop leaves open") {
    const int n = 12;
    MultiresSurface s = build(closed_torus(n, n));
    MultiresError err = MultiresError::None;
    REQUIRE(s.refine_patches_to_level(torus_block(n, 1, 2, 1, 2), 3, &err));

    // THE CONTROL, and it is the loop `clay.h` tells a host to write:
    // `build_block(effective_level(patch, display), patch)` per patch. It is
    // not cracked by a hairline — the gap is a subdivision step — so welding at
    // exactly zero leaves these edges with one triangle instead of two.
    const std::size_t expected_open[4] = {0, 72, 168, 264};
    const std::size_t expected_vertices[4] = {144, 264, 472, 680};
    for (std::uint32_t display = 0; display <= 3; ++display) {
        INFO("display " << display);
        const Assembled today = assemble_patches(s, display, false);
        CHECK(open_edges(today.indices) == expected_open[display]);

        // The same loop over the mixed blocks closes it, and the reason is
        // identity rather than proximity: the shared vertex is the same
        // (level, id) in both patches, so the weld above had nothing to do.
        const Assembled fixed = assemble_patches(s, display, true);
        CHECK(open_edges(fixed.indices) == 0u);
        CHECK(fixed.vertices == expected_vertices[display]);

        // And the whole-surface export is the same surface, closed without any
        // welding at all — its shared vertices are one index to begin with.
        mesh::MultiresMixedStatus status = mesh::MultiresMixedStatus::NotBuilt;
        const Mesh whole = s.mixed_mesh_at_level(display, {}, &status);
        CHECK(status == mesh::MultiresMixedStatus::Ok);
        CHECK(whole.positions.size() == expected_vertices[display]);
        CHECK(open_edges(whole.indices) == 0u);
    }
}

TEST_CASE("regional export: a uniform-depth export is the level's own mesh, byte for byte") {
    // Nothing to bridge means nothing to change, and this is the parity gate
    // for it: a hierarchy with one depth exports what it always exported.
    MultiresSurface uniform = build(closed_torus(6, 6));
    for (int l = 0; l < 2; ++l) REQUIRE(uniform.add_level());
    const Mesh single = uniform.mesh_at_level(2);
    const Mesh mixed = uniform.mixed_mesh_at_level(2);
    REQUIRE(single.positions.size() == 576u);
    REQUIRE(single.normals.size() == single.positions.size());  // not a vacuous comparison
    CHECK(same_mesh(single, mixed));
    CHECK(mixed.quads.size() / 4u * 6u == mixed.indices.size());

    // The same statement on a REGIONAL hierarchy read at a display level every
    // patch reaches: the depths agree there, so the export is the level again.
    const int n = 12;
    MultiresSurface s = build(closed_torus(n, n));
    REQUIRE(s.refine_patches_to_level(torus_block(n, 1, 2, 1, 2), 3));
    CHECK(same_mesh(s.mesh_at_level(0), s.mixed_mesh_at_level(0)));
}

TEST_CASE("regional export: quads survive exactly as far as the split edges allow") {
    // A COARSE QUAD WITH ONE SPLIT EDGE IS A PENTAGON, AND A PENTAGON HAS NO
    // QUADRANGULATION. Four edges per quad counts every interior edge twice, so
    // a quadrangulated polygon's boundary vertex count must be EVEN whatever is
    // added inside it. Making it even would mean splitting a second edge of
    // that face, which its coarse neighbour must then also carry, and so on
    // across the model. So the export keeps `Mesh::quads` exactly while no edge
    // is split and drops it otherwise — never a quad list that does not
    // describe `indices`, which `mesh_data.h` forbids.
    MultiresSurface uniform = build(closed_torus(6, 6));
    for (int l = 0; l < 2; ++l) REQUIRE(uniform.add_level());
    const Mesh flat = uniform.mixed_mesh_at_level(2);
    CHECK_FALSE(flat.quads.empty());
    CHECK(flat.quads.size() / 4u * 6u == flat.indices.size());

    const int n = 12;
    MultiresSurface s = build(closed_torus(n, n));
    REQUIRE(s.refine_patches_to_level(torus_block(n, 1, 2, 1, 2), 3));
    const Mesh mixed = s.mixed_mesh_at_level(3);
    CHECK(mixed.quads.empty());

    // WHICH PATCHES PAY FOR IT, as three counts. A patch with a split edge
    // emits more triangles than twice its faces. A CORNER-ONLY patch — one that
    // meets the refined region at a cage vertex and shares no edge with it —
    // emits exactly twice its faces and STILL spans two levels, because its
    // corner took the fine side's value. That is the ripple: it reaches faces
    // with no T-junction at all, and if it did not the crack would simply move
    // one face inwards.
    std::size_t split = 0, corner_only = 0, untouched = 0;
    MultiresSurface::Block b;
    for (std::uint32_t p = 0; p < static_cast<std::uint32_t>(n * n); ++p) {
        REQUIRE(s.build_mixed_block(3, p, &b));
        const std::size_t quads_worth = 2u * patch_face_count(s, b.level, p);
        if (b.indices.size() / 3 > quads_worth) ++split;
        else if (!b.vertex_levels.empty()) ++corner_only;
        else ++untouched;
    }
    CHECK(split == 48u);
    CHECK(corner_only == 12u);
    CHECK(untouched == 84u);
}

TEST_CASE("regional export: asking in the other order exports the same bytes") {
    // Determinism needs no new rule here — the levels are built in ascending
    // patch order and `full_of` is ascending, so the export's own numbering
    // (ascending level, then ascending vertex) is a function of the topology.
    // Asked in the opposite order, as `encode()`'s own determinism gate is.
    const int n = 12;
    const std::vector<std::uint32_t> region = torus_block(n, 1, 2, 1, 2);
    std::vector<std::uint32_t> reversed(region.rbegin(), region.rend());

    MultiresSurface a = build(closed_torus(n, n));
    REQUIRE(a.refine_patches_to_level(region, 3));
    MultiresSurface b = build(closed_torus(n, n));
    REQUIRE(b.refine_patches_to_level(reversed, 3));
    const Mesh from_ascending = a.mixed_mesh_at_level(3);
    const Mesh from_descending = b.mixed_mesh_at_level(3);
    REQUIRE(from_ascending.positions.size() == 680u);
    CHECK(same_mesh(from_ascending, from_descending));
}

TEST_CASE("regional export: refining a distant region leaves this one byte-identical") {
    // Refinement is monotonic, so a transition set can only shrink, and the
    // guarantee that follows is the one an incremental host needs: a coarse
    // face whose vertex ring's residency did not change emits the same faces.
    // Compared between two hierarchies rather than across a mutation, because
    // `refine_patches_to_level` builds levels rather than growing the ones that
    // exist — asking the same question in the form the API can actually be
    // asked it.
    const int n = 12;
    const std::vector<std::uint32_t> here = torus_block(n, 1, 2, 1, 2);
    std::vector<std::uint32_t> both = here;
    const std::vector<std::uint32_t> elsewhere = torus_block(n, 7, 8, 7, 8);
    both.insert(both.end(), elsewhere.begin(), elsewhere.end());

    MultiresSurface one = build(closed_torus(n, n));
    REQUIRE(one.refine_patches_to_level(here, 3));
    MultiresSurface two = build(closed_torus(n, n));
    REQUIRE(two.refine_patches_to_level(both, 3));

    std::size_t identical = 0, changed = 0;
    MultiresSurface::Block x, y;
    for (std::uint32_t p = 0; p < static_cast<std::uint32_t>(n * n); ++p) {
        REQUIRE(one.build_mixed_block(3, p, &x));
        REQUIRE(two.build_mixed_block(3, p, &y));
        const bool same = x.level == y.level && x.indices == y.indices &&
                          x.vertex_levels.size() == y.vertex_levels.size() &&
                          same_floats(block_positions(one, x), block_positions(two, y));
        if (same) ++identical; else ++changed;
    }
    CHECK(identical == 84u);
    // NOT VACUOUS: the far region and its grading really did move. If this were
    // 0 the count above would prove nothing.
    CHECK(changed == 60u);

    // And the transition beside the FIRST region — the one with pentagons in it
    // — is in the identical half, positions included.
    REQUIRE(one.build_mixed_block(3, 1, &x));
    REQUIRE(two.build_mixed_block(3, 1, &y));
    CHECK(x.indices.size() / 3 == 36u);  // 16 faces, 4 of them split
    CHECK(x.indices == y.indices);
    CHECK(same_floats(block_positions(one, x), block_positions(two, y)));
}

TEST_CASE("regional export: a split cage is refused by name rather than exported wrong") {
    // The attribute hierarchy is a SECOND topology per level, mapped to the
    // geometric one face for face. A mixed-depth face has corners from two
    // levels and, at a split edge, a corner neither level's face carries, so
    // there is no counterpart to read a seam's two values through. Refused with
    // a name — never silently welded to one side, and never quietly dropped.
    Mesh seam;
    seam.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(1, 0, 1), cf3(0, 0, 1),
                      cf3(1, 0, 0), cf3(2, 0, 0), cf3(2, 0, 1), cf3(1, 0, 1)};
    seam.uvs = {cf2(0, 0), cf2(1, 0), cf2(1, 1), cf2(0, 1),
                cf2(0, 0), cf2(1, 0), cf2(1, 1), cf2(0, 1)};
    seam.quads = {0, 1, 2, 3, 4, 5, 6, 7};
    seam.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    MultiresSurface s = build(seam);
    REQUIRE(s.refine_patches_to_level({0}, 2));
    REQUIRE(s.patch_max_level(0) == 2u);
    REQUIRE(s.patch_max_level(1) == 1u);

    mesh::MultiresMixedStatus status = mesh::MultiresMixedStatus::Ok;
    const Mesh refused = s.mixed_mesh_at_level(2, {}, &status);
    CHECK(status == mesh::MultiresMixedStatus::AttributeSplitCage);
    CHECK(refused.positions.empty());

    mesh::MultiresExportOptions geometry;
    geometry.uvs = false;
    geometry.colors = false;
    const Mesh allowed = s.mixed_mesh_at_level(2, geometry, &status);
    CHECK(status == mesh::MultiresMixedStatus::Ok);
    CHECK(allowed.positions.size() == 31u);
    // THE CAGE'S OWN RIM AND NOTHING ELSE. This cage is a two-quad strip with
    // a real boundary, so "closed" is not the assertion here: 12 edges around
    // the level-2 patch's three outer sides plus 6 around the level-1 patch's
    // three. The shared edge between them, which is the transition, is not
    // among them.
    CHECK(open_edges(allowed.indices) == 18u);

    // A cage that does NOT split its attributes carries them through, per
    // emitted vertex, from the level that vertex lives at.
    Mesh coloured = closed_torus(6, 6);
    coloured.colors.resize(coloured.positions.size());
    for (std::size_t i = 0; i < coloured.colors.size(); ++i)
        coloured.colors[i] = cf3(static_cast<float>(i) * 0.01f, 0.5f, 0.75f);
    MultiresSurface c = build(coloured);
    REQUIRE(c.refine_patches_to_level({7, 8, 13, 14}, 2));
    const Mesh with_colour = c.mixed_mesh_at_level(2, {}, &status);
    CHECK(status == mesh::MultiresMixedStatus::Ok);
    CHECK(with_colour.colors.size() == with_colour.positions.size());
    CHECK(with_colour.normals.size() == with_colour.positions.size());
}

TEST_CASE("regional export: the export is a read") {
    // The question ClaySpaceDesktop asked of this half, answered as a gate: a
    // path that produces a sculpted level's geometry must not cost a document
    // edit. It evaluates — exactly the levels `mesh_at_level` evaluates — and
    // it writes nothing.
    const int n = 12;
    MultiresSurface s = build(closed_torus(n, n));
    REQUIRE(s.refine_patches_to_level(torus_block(n, 1, 2, 1, 2), 3));
    const std::uint64_t detail = s.detail_checksum();
    const std::uint64_t base_rev = s.base_revision();
    const std::uint64_t detail_rev = s.detail_revision();

    const Mesh once = s.mixed_mesh_at_level(3);
    const Mesh twice = s.mixed_mesh_at_level(3);
    CHECK(s.detail_checksum() == detail);
    CHECK(s.base_revision() == base_rev);
    CHECK(s.detail_revision() == detail_rev);
    CHECK(same_mesh(once, twice));
}

TEST_CASE("regional export: the transition is derived, so the stream never learns about it") {
    // `multires_serialize.cpp` writes the per-level PATCH SETS and replays the
    // build; the face lists, `full_of` and the chunk tables are all rebuilt. A
    // transition is a function of the cage, the rule and those patch sets, so
    // it needs no bytes — which is what keeps "transition geometry SHALL NOT
    // become the authoritative sculpt representation" a structural fact rather
    // than a discipline, and what keeps an older build able to read the
    // document.
    const int n = 12;
    MultiresSurface s = build(closed_torus(n, n));
    REQUIRE(s.refine_patches_to_level(torus_block(n, 1, 2, 1, 2), 3));

    const std::vector<std::uint8_t> before = s.encode();
    const Mesh exported = s.mixed_mesh_at_level(3);
    // EXPORTING WROTE NOTHING INTO THE DOCUMENT. Not a size comparison: the
    // same bytes.
    CHECK(s.encode() == before);

    MultiresSurface reloaded;
    REQUIRE(MultiresSurface::decode(before.data(), before.size(), &reloaded));
    const Mesh after_round_trip = reloaded.mixed_mesh_at_level(3);
    CHECK(same_mesh(exported, after_round_trip));
    CHECK(open_edges(after_round_trip.indices) == 0u);
}

TEST_CASE("regional export: a mixed export costs no more resident memory than a single-level one") {
    // TASK 5.8, DECIDED AGAINST A NUMBER RATHER THAN AN ASSUMPTION: does a
    // mixed-depth export need its own preflight?
    //
    // The worry is real in shape — a mixed export reads several levels at once,
    // so it forces levels 0..n simultaneously resident, and that is the
    // peak-versus-persistent argument `preflight_add_level` exists for. It does
    // not apply here, and the reason is that `mesh_at_level` ALREADY walks
    // every level below its own: `evaluate_up_to(level)` is the first thing
    // both calls do. The mixed export then reads the evaluated positions
    // directly and builds no level mesh, no adjacency and no chunk table, so
    // its resident set is a SUBSET of the one the existing export leaves
    // behind. No preflight is added, and this is the measurement that says so.
    const int n = 12;
    const std::vector<std::uint32_t> region = torus_block(n, 1, 2, 1, 2);
    MultiresSurface single = build(closed_torus(n, n));
    REQUIRE(single.refine_patches_to_level(region, 3));
    MultiresSurface mixed = build(closed_torus(n, n));
    REQUIRE(mixed.refine_patches_to_level(region, 3));
    const mesh::MultiresMemory cold = mixed.memory();

    const Mesh from_level = single.mesh_at_level(3);
    const Mesh from_mixed = mixed.mixed_mesh_at_level(3);
    REQUIRE_FALSE(from_level.positions.empty());
    REQUIRE_FALSE(from_mixed.positions.empty());

    const mesh::MultiresMemory after_single = single.memory();
    const mesh::MultiresMemory after_mixed = mixed.memory();
    CHECK(after_mixed.rebuildable <= after_single.rebuildable);
    CHECK(after_mixed.authoritative == after_single.authoritative);
    // NOT VACUOUS: exporting did make the surface bigger, so "no more than" is
    // a comparison of two real numbers rather than of two zeros.
    CHECK(after_mixed.rebuildable > cold.rebuildable);
}
