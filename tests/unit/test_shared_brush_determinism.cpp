// WHAT THE ARENA MAY NOT CHANGE (meshing spec, add-shared-brush-runtime).
//
// THE DEFECT CLASS THIS FILE EXISTS FOR, and it is the one the arena refactor
// could have introduced everywhere at once. Task 2.4 and 2.5 replaced per-stamp
// `std::vector`s with blocks handed out by `BrushScratchArena`, and the two are
// not the same thing in one respect that no other test in the tree can see: a
// `std::vector<int> depth(count, -1)` is VALUE-INITIALIZED at construction and
// an arena block is not. `reset()` is a pointer store — it explicitly runs no
// destructors and writes no bytes — so the storage a stamp is handed is
// whatever the PREVIOUS stamp left in it.
//
// A path that reads any of that scratch before writing it is therefore correct
// on a cold arena and wrong on a warm one, and the whole rest of the suite runs
// on cold arenas: every fixture builds its sculptor, stamps once or a few times
// at one size, and reads the answer. `test_sculpt_allocation.cpp` counts
// allocations and would not notice. The parity files compare representations
// against each other, and two sculptors that had both read the same fresh
// zeroed page would agree perfectly.
//
// SO THE GATE IS HISTORY-INDEPENDENCE, STATED AS AN EQUALITY: the same stamp on
// the same geometry writes the same bytes whether the sculptor's arena is cold,
// or has been grown and dirtied by a much larger stamp first. The dirtying is
// done through the ordinary API — a big automasked stamp, whose geometric
// effect is then undone exactly — rather than by reaching into the arena, both
// because `arena()` is const by design and because a stroke that changes size
// is what actually happens to an artist.
//
// IT IS NOT A TAUTOLOGY AND THE FILE CHECKS THAT IT IS NOT: every case asserts
// that the warmed arena really is larger than the cold one before it compares
// anything, so a refactor that stopped the arena growing would fail here rather
// than quietly turn these into two identical runs.
//
// AND ONE MORE CLAIM, ON THE SAME FIXTURES BECAUSE IT NEEDS THE SAME ONES.
// Task 4.4 says a fully masked entry is dropped from the workset "so it is
// bit-identical to its input rather than merely close". `merely close` is what
// a multiply by 0.0f gives you — it is exact for a finite displacement, but the
// vertex still goes through the write path, still lands in the write region,
// still counts as moved and still costs an undo record entry. The difference is
// invisible in a position comparison and visible in the write region, so that
// is where it is asserted.
//
// EVERY FIXTURE COORDINATE IS A POWER OF TWO OR A SUM OF THEM, as
// `test_dynamic_shared_brush_parity.cpp` records: the fixture must not be a
// second source of rounding that the byte comparisons would then be measuring.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::AutomaskFactor;
using mesh::AutomaskInputs;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::DynamicVertex;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;
using mesh::VertexDeltas;
using mesh::VertexId;

namespace {

// Rippled rather than flat, for the reason the parity files give: on a flat
// grid the Laplacian of an interior vertex is the vertex itself, and half the
// verbs would contribute an equality that pins a no-op.
float ripple_height(int x, int z) {
    static const float kWave[8] = {0.0f,     0.0625f,  0.125f,  0.0625f,
                                   0.0f,     -0.0625f, -0.125f, -0.0625f};
    return kWave[(x + z) & 7];
}

Mesh plane_grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), ripple_height(x, z),
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

bool same_bits(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(cfloat3)) == 0;
}

// The three input-free factors together. `Boundary` is the one that makes the
// arena work on every entry — it is what allocates `depth`, `frontier`, `next`
// and `ring` — and `TopologyConnected` is what allocates `reached` and `stack`.
// A case built on `NormalAngle` alone would dirty nothing and prove nothing.
constexpr std::uint32_t kArenaFactors =
    static_cast<std::uint32_t>(AutomaskFactor::Boundary) |
    static_cast<std::uint32_t>(AutomaskFactor::TopologyConnected) |
    static_cast<std::uint32_t>(AutomaskFactor::NormalAngle);

MeshBrushSettings measured_brush() {
    MeshBrushSettings b;
    b.center = cf3(0.25f, 0.0f, -0.125f);
    b.radius = 0.5f;
    b.strength = 0.25f;
    b.direction = cf3(0.0f, 0.5f, 0.0f);
    b.geodesic = false;
    b.automask.factors = kArenaFactors;
    b.automask.boundary_rings = 3;
    b.automask.normal_angle = 1.0f;
    return b;
}

// The stamp whose only job is to make the arena big and leave rubbish in it: a
// footprint over the whole grid, so every scratch block is sized by the whole
// workset and every one of them is written full of live indices and depths.
MeshBrushSettings dirtying_brush() {
    MeshBrushSettings b = measured_brush();
    b.center = cf3(0.0f, 0.0f, 0.0f);
    b.radius = 4.0f;
    b.automask.boundary_rings = 12;
    return b;
}

}  // namespace

TEST_CASE("arena history: a dirtied arena stamps exactly what a cold one does (fixed mesh)") {
    Mesh cold_mesh = plane_grid(24, 1.0f);
    Mesh warm_mesh = plane_grid(24, 1.0f);
    const std::vector<cfloat3> original = cold_mesh.positions;
    REQUIRE(same_bits(cold_mesh.positions, warm_mesh.positions));

    MeshSculptor cold(cold_mesh, 0.0f);
    MeshSculptor warm(warm_mesh, 0.0f);

    // Grow and dirty the warm arena, then take the geometry back to where it
    // started. `VertexDeltas::revert` is documented bit-exact and gated by
    // `test_mesh_sculpt.cpp`; the REQUIRE below is what makes this test's own
    // comparison meaningful rather than trusting that.
    VertexDeltas undo;
    const std::size_t dirtied = warm.stamp(MeshBrush::Draw, dirtying_brush(), {}, &undo);
    REQUIRE(dirtied > 0);
    REQUIRE_FALSE(same_bits(warm_mesh.positions, original));
    REQUIRE(undo.revert(warm_mesh));
    REQUIRE(same_bits(warm_mesh.positions, original));
    REQUIRE(same_bits(warm_mesh.normals, cold_mesh.normals));

    // THE ANTI-TAUTOLOGY. If the warm arena were not actually bigger than the
    // cold one is about to be, the two runs below would be the same run twice.
    const std::size_t warmed_capacity = warm.arena().capacity_bytes();
    REQUIRE(warmed_capacity > 0);

    // A stroke rather than a stamp, so the arena is also reused BETWEEN dabs
    // and a path that depends on the previous dab's leavings fails here too.
    const MeshBrushSettings base = measured_brush();
    std::vector<std::size_t> cold_moved, warm_moved;
    for (int i = 0; i < 6; ++i) {
        MeshBrushSettings b = base;
        b.center = cf3(0.25f - 0.0625f * static_cast<float>(i), 0.0f, -0.125f);
        cold_moved.push_back(cold.stamp(MeshBrush::Draw, b));
        warm_moved.push_back(warm.stamp(MeshBrush::Draw, b));
    }

    CHECK(cold.arena().capacity_bytes() < warmed_capacity);
    CHECK(cold_moved == warm_moved);
    CHECK(std::count(cold_moved.begin(), cold_moved.end(), std::size_t{0}) == 0);
    CHECK(same_bits(cold_mesh.positions, warm_mesh.positions));
    CHECK(same_bits(cold_mesh.normals, warm_mesh.normals));

    // The write regions agree entry for entry, which is the claim a position
    // comparison cannot make: two runs could write the same bytes while
    // disagreeing about which vertices they had touched.
    REQUIRE(cold.write_region().size() == warm.write_region().size());
    for (std::size_t i = 0; i < cold.write_region().size(); ++i)
        CHECK(cold.write_region()[i].key() == warm.write_region()[i].key());
}

TEST_CASE("arena history: a dirtied arena stamps exactly what a cold one does (adaptive)") {
    const Mesh source = plane_grid(20, 1.0f);
    auto cold_surface = DynamicSurface::from_mesh(source);
    auto warm_surface = DynamicSurface::from_mesh(source);
    REQUIRE(cold_surface.has_value());
    REQUIRE(warm_surface.has_value());

    // The adaptive surface has no `VertexDeltas`, so the restore is a snapshot
    // of the slots themselves. Positions AND normals: a stamp refreshes the
    // normals of what it touched, and leaving those moved would make the second
    // run a stamp on a different surface rather than a stamp with a different
    // arena.
    std::vector<VertexId> ids;
    std::vector<cfloat3> saved_positions, saved_normals;
    warm_surface->vertices().for_each_live([&](VertexId id, const DynamicVertex& v) {
        ids.push_back(id);
        saved_positions.push_back(v.position);
        saved_normals.push_back(v.normal);
    });
    REQUIRE(ids.size() == source.positions.size());

    DynamicSculptor cold(*cold_surface);
    DynamicSculptor warm(*warm_surface);
    DynamicTopologySettings topo;
    topo.enabled = false;  // a stable surface, so the two runs hold the same slots

    const mesh::DynamicStampResult dirtied =
        warm.stamp(MeshBrush::Draw, dirtying_brush(), topo);
    REQUIRE(dirtied.moved_vertices > 0);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        DynamicVertex* dv = warm_surface->vertex(ids[i]);
        REQUIRE(dv != nullptr);
        dv->position = saved_positions[i];
        dv->normal = saved_normals[i];
    }

    const std::size_t warmed_capacity = warm.arena().capacity_bytes();
    REQUIRE(warmed_capacity > 0);

    const MeshBrushSettings base = measured_brush();
    for (int i = 0; i < 6; ++i) {
        MeshBrushSettings b = base;
        b.center = cf3(0.25f - 0.0625f * static_cast<float>(i), 0.0f, -0.125f);
        const mesh::DynamicStampResult c = cold.stamp(MeshBrush::Draw, b, topo);
        const mesh::DynamicStampResult w = warm.stamp(MeshBrush::Draw, b, topo);
        CAPTURE(i);
        REQUIRE(c.moved_vertices > 0);
        CHECK(c.moved_vertices == w.moved_vertices);
        CHECK(c.remesh.total() == w.remesh.total());
        // NOT `geometry_revision`: it is a monotone counter over the surface's
        // whole life, and the warm sculptor has one more stamp behind it. The
        // first version of this case compared it and was measuring the warm-up.
        CHECK(c.dirty_bounds.min.x == w.dirty_bounds.min.x);
        CHECK(c.dirty_bounds.max.y == w.dirty_bounds.max.y);
    }

    CHECK(cold.arena().capacity_bytes() < warmed_capacity);

    std::vector<cfloat3> cold_out, warm_out;
    for (VertexId v : ids) {
        cold_out.push_back(cold_surface->vertex(v)->position);
        warm_out.push_back(warm_surface->vertex(v)->position);
    }
    CHECK(same_bits(cold_out, warm_out));

    CHECK(cold.last_region().size() == warm.last_region().size());
}

// THE OTHER HALF OF 4.4, and the half a position comparison cannot see.
//
// An entry the automask took to exactly zero could be left in the workset and
// multiplied by 0.0f, and every position in the mesh would still be right — the
// product is exact. What would NOT be right is everything downstream that reads
// "what did this stamp write": the write region a host uploads, the moved count
// it reports, and the undo record it costs. So the assertion is on the region
// and on the record, and the positions are checked only to confirm that the
// masked vertices really were the ones left alone.
TEST_CASE("automask: a fully masked vertex is not in the write region at all") {
    Mesh masked_mesh = plane_grid(24, 1.0f);
    Mesh open_mesh = plane_grid(24, 1.0f);
    const std::vector<cfloat3> original = masked_mesh.positions;
    const std::vector<cfloat3> original_normals = masked_mesh.normals;

    MeshSculptor masked(masked_mesh, 0.0f);
    MeshSculptor open(open_mesh, 0.0f);

    // A world lattice naming the half-space x >= 0, which is what a document's
    // group field is: the estimator lives above `mesh` and arrives as a
    // callback, so a test can be exact about where the boundary falls.
    AutomaskInputs inputs;
    inputs.group = [](cfloat3 p) -> std::uint32_t { return p.x >= 0.0f ? 7u : 3u; };
    inputs.active_group = 7;
    masked.set_automask_inputs(inputs);

    MeshBrushSettings b;
    b.center = cf3(0.0f, 0.0f, 0.0f);
    b.radius = 0.75f;
    b.strength = 0.5f;
    b.direction = cf3(0.0f, 0.5f, 0.0f);
    b.geodesic = false;

    const std::size_t open_moved = open.stamp(MeshBrush::Draw, b);
    b.automask.factors = static_cast<std::uint32_t>(AutomaskFactor::SurfaceGroup);
    VertexDeltas undo;
    const std::size_t masked_moved = masked.stamp(MeshBrush::Draw, b, {}, &undo);

    // The stamp has to straddle the boundary, or the case is satisfied by a
    // brush that never reached the masked half.
    REQUIRE(open_moved > 0);
    REQUIRE(masked_moved > 0);
    CHECK(masked_moved < open_moved);

    // THE CLAIM, SAID WHERE IT IS ACTUALLY VISIBLE: the masked entry is not in
    // the WORKSET. Measured before this assertion existed, on the whole 2089
    // case suite with the drop replaced by a keep-and-multiply-by-zero (which
    // compiles): exactly one case noticed, and it noticed incidentally, by
    // reading a weight it was checking for another reason.
    //
    // The write region is not where it shows, which is the trap this case fell
    // into first. A kept entry at weight 0.0f produces a displacement of
    // exactly zero, so it never enters the write region and never reaches the
    // undo record either — the two places the first version of this test
    // looked, both of which passed against the reverted engine. What a kept
    // entry DOES do is sit in `items` with a zero weight, hold a live slot in
    // the map its neighbours are looked up through, and cost every later pass
    // its share of the loop.
    for (std::size_t i = 0; i < masked.workset().size(); ++i) {
        const std::uint32_t cls = masked.workset().items[i].as_weld_class();
        REQUIRE(cls < masked_mesh.positions.size());
        CHECK(inputs.group(original[cls]) == 7u);
        // And every surviving weight is strictly positive, which is the same
        // statement from the other side and the one a future factor that masks
        // by something other than a group would also have to satisfy.
        CHECK(masked.workset().weights[i] > 0.0f);
        CHECK(masked.workset().slot[cls] == i);
    }
    // The unmasked stamp reached strictly more, so the workset really was
    // shortened rather than merely all-in-group by luck of where the brush sat.
    CHECK(masked.workset().size() < open.workset().size());

    // Every entry of the write region is in the workset, which is the subset
    // relation the header states and the one thing the drop must not break.
    for (const mesh::WorkItemId& item : masked.write_region()) {
        const std::uint32_t cls = item.as_weld_class();
        REQUIRE(cls < masked_mesh.positions.size());
        CHECK(inputs.group(original[cls]) == 7u);
        CHECK(masked.workset().slot[cls] != mesh::kNoClass);
    }
    // What the mask held is byte-identical to its input, not merely close.
    std::size_t held = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (inputs.group(original[i]) == 7u) continue;
        ++held;
        CHECK(std::memcmp(&masked_mesh.positions[i], &original[i], sizeof(cfloat3)) == 0);
    }
    CHECK(held > 0);

    // THE UNDO RECORD IS LARGER THAN THE WRITE REGION, AND THAT IS CORRECT.
    // Measured here at 183 against 133 on this fixture, and the first version
    // of this case asserted they were equal, which was measuring the wrong
    // quantity: `VertexDeltas` stores normals rather than recomputing them on
    // revert (its own header says why — an imported mesh's normals are whatever
    // its author wrote), so a vertex whose NORMAL was refreshed because a
    // neighbour moved has to be in the record even though it never moved
    // itself. Fifty of those sit in the masked half, one ring outside the write
    // region.
    //
    // So the claim is the sharp one rather than the tidy one: every entry in
    // the record changed SOMETHING, and everything outside it changed nothing.
    // That is a statement about the undo COST — a record is what a host keeps
    // and what a session journal writes — and not about the automask drop,
    // which was measured above and does not reach here.
    CHECK(undo.size() > masked.write_region().size());
    std::vector<bool> in_record(original.size(), false);
    for (std::uint32_t v : undo.vertices()) {
        REQUIRE(v < original.size());
        in_record[v] = true;
        const bool moved = std::memcmp(&masked_mesh.positions[v], &original[v],
                                       sizeof(cfloat3)) != 0;
        const bool renormalled =
            std::memcmp(&masked_mesh.normals[v], &original_normals[v], sizeof(cfloat3)) != 0;
        CHECK((moved || renormalled));
        // And a record entry outside the active group is one the automask held:
        // it may have a fresh normal, it may not have a fresh position.
        if (inputs.group(original[v]) != 7u) CHECK_FALSE(moved);
    }
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (in_record[i]) continue;
        CHECK(std::memcmp(&masked_mesh.positions[i], &original[i], sizeof(cfloat3)) == 0);
        CHECK(std::memcmp(&masked_mesh.normals[i], &original_normals[i], sizeof(cfloat3)) == 0);
    }

    // UNDO EXACTNESS UNDER AN AUTOMASK, on both attributes the record carries.
    // A revert that restored a vertex the stamp never wrote would show as a
    // change here rather than as a missing one.
    REQUIRE(undo.revert(masked_mesh));
    CHECK(same_bits(masked_mesh.positions, original));
    CHECK(same_bits(masked_mesh.normals, original_normals));
}
