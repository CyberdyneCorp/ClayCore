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

#include <cstdint>
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
