// THE SHARED BRUSH RUNTIME, AGAINST THE HIERARCHY (meshing and brush-engine
// specs, add-shared-brush-runtime 6.5).
//
// The companion to `test_dynamic_shared_brush_parity.cpp`, and the row where
// parity means something slightly different, so it is worth saying what before
// asserting it.
//
// AT THE CAGE the hierarchy IS the fixed sculptor: `MultiresSculptor::stamp`
// binds a `MeshSculptor` to level 0's own mesh and calls it. So P3 is bit-exact
// there and the interesting question is not whether the arithmetic agrees — it
// is the same instructions — but whether the NEUTRAL RUNTIME between the two
// still says the same thing: whether `build_multires_workset` re-tags the items
// as (level, vertex) without disturbing a weight, a position or a normal, and
// whether the arena it reports is the one that actually grew.
//
// ABOVE THE CAGE parity is exact to a FRAME ROUND TRIP and cannot be bit-exact,
// and that is the representation working rather than failing: what the brush
// wrote is stored as a detail coefficient in a transported frame and read back
// out of it, which is precisely what keeps the surface a host sees identical to
// the surface a reload produces. That is P4 for this representation, and it is
// asserted AS a difference with a stated bound — a level that suddenly became
// bit-exact would mean the detail had stopped going through the frame.
//
// WHAT THIS FILE DELIBERATELY DOES NOT RE-ASSERT. `test_multires_sculpt.cpp`
// already gates that a fine stamp becomes detail, that detail survives a coarse
// edit, and that a gesture reverts. Those are the representation's own
// contract; this file is about the shared runtime underneath it.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/layered_sculpt.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/sculpt_kernels.h"

using namespace clay;
using namespace clay::kernel;
using mesh::AutomaskFactor;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::DynamicVertex;
using mesh::LayeredMultiresSculptor;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;
using mesh::MultiresError;
using mesh::MultiresSculptor;
using mesh::MultiresSurface;
using mesh::SculptLayerId;
using mesh::SculptSnapshot;
using mesh::SculptWorkset;
using mesh::VertexId;

namespace {

bool same_bits(cfloat3 a, cfloat3 b) { return std::memcmp(&a, &b, sizeof(cfloat3)) == 0; }

// A QUAD cage, because that is what a Catmull-Clark hierarchy is built from,
// carrying its triangulation too — so ONE source model serves all three
// representations and P3 is a claim about the same surface rather than about
// three surfaces that resemble each other. Every coordinate is a power of two.
Mesh plane_quads(int n, float half) {
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
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

MultiresSurface build(const Mesh& m, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(m, {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

MeshBrushSettings cage_brush() {
    MeshBrushSettings b;
    b.center = cf3(0, 0, 0);
    b.radius = 0.75f;
    b.strength = 0.5f;
    b.geodesic = false;
    b.direction = cf3(0.125f, 0.25f, 0.0f);
    return b;
}

DynamicTopologySettings topology_off() {
    DynamicTopologySettings t;
    t.enabled = false;
    return t;
}

}  // namespace

// -- P1 and P2 ----------------------------------------------------------------

TEST_CASE("multires parity P1/P2: the hierarchy's workset is the level's, re-tagged") {
    // THE NEUTRAL WORKSET'S WHOLE CLAIM, at the representation whose identity
    // is a PAIR. `build_multires_workset` takes the bound level sculptor's
    // workset and re-tags every item as (level, vertex); if it disturbed a
    // weight, a position or a normal on the way through, every kernel above it
    // would read a different snapshot and nothing else in the suite would say
    // so — the positions would still be plausible.
    MultiresSurface s = build(plane_quads(8, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(0));

    Mesh flat = s.mesh_at_level(0, {/*normals=*/false, false, false});
    flat.normals = s.normals_at(0);
    MeshSculptor fixed(flat, 0.0f);
    MultiresSculptor hierarchy(s);

    const MeshBrushSettings b = cage_brush();
    const std::size_t moved_hierarchy = hierarchy.stamp(MeshBrush::Grab, b);
    const std::size_t moved_fixed = fixed.stamp(MeshBrush::Grab, b);
    REQUIRE(moved_hierarchy == moved_fixed);
    REQUIRE(moved_hierarchy == 9);

    const SculptWorkset& h = hierarchy.workset();
    const SculptWorkset& f = fixed.workset();
    REQUIRE(h.size() == f.size());
    REQUIRE(h.size() == 9);

    for (std::size_t i = 0; i < h.size(); ++i) {
        CAPTURE(i);
        // THE DENSE HALF IS THE LEVEL VERTEX and the HIGH half is the level —
        // which is what makes `slot[item.key()]` one correct spelling on all
        // three representations, and is the encoding rule the whole neutral
        // composition rests on.
        CHECK(h.items[i].key() == f.items[i].key());
        CHECK(h.items[i].level() == 0u);
        CHECK(h.items[i].level_vertex_index() == f.items[i].as_weld_class());

        CHECK(same_bits(h.positions[i], f.positions[i]));
        CHECK(same_bits(h.normals[i], f.normals[i]));
        CHECK(std::memcmp(&h.weights[i], &f.weights[i], sizeof(float)) == 0);  // P2
    }

    CHECK(same_bits(h.average_normal, f.average_normal));
    CHECK(same_bits(h.centroid, f.centroid));
    CHECK(same_bits(h.plane_point, f.plane_point));
    CHECK(same_bits(h.plane_normal, f.plane_normal));
}

TEST_CASE("multires parity: the item's level is the level it was stamped at") {
    // The other half of the (level, vertex) claim, which the level-0 case
    // cannot make: a level tag that was hard-wired to zero would pass
    // everything above.
    MultiresSurface s = build(plane_quads(8, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor hierarchy(s);

    MeshBrushSettings b = cage_brush();
    b.radius = 0.5f;
    REQUIRE(hierarchy.stamp(MeshBrush::Grab, b) > 0);

    const SculptWorkset& w = hierarchy.workset();
    REQUIRE(w.size() > 20);
    for (std::size_t i = 0; i < w.size(); ++i) {
        CAPTURE(i);
        CHECK(w.items[i].level() == 2u);
    }

    // And the vertex halves are all distinct, which is what says the pair is
    // an identity rather than a decoration.
    std::vector<std::uint32_t> keys;
    for (std::size_t i = 0; i < w.size(); ++i) keys.push_back(w.items[i].key());
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE("multires parity P1: every kernel agrees on the two snapshots") {
    MultiresSurface s = build(plane_quads(8, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(0));

    Mesh flat = s.mesh_at_level(0, {/*normals=*/false, false, false});
    flat.normals = s.normals_at(0);
    MeshSculptor fixed(flat, 0.0f);
    MultiresSculptor hierarchy(s);

    const MeshBrushSettings b = cage_brush();
    REQUIRE(hierarchy.stamp(MeshBrush::Grab, b) > 0);
    REQUIRE(fixed.stamp(MeshBrush::Grab, b) > 0);

    auto snapshot_of = [](const SculptWorkset& w) {
        SculptSnapshot snap;
        snap.positions = w.positions.data();
        snap.normals = w.normals.data();
        snap.weights = w.weights.data();
        snap.count = w.size();
        snap.average_normal = w.average_normal;
        snap.centroid = w.centroid;
        snap.plane_point = w.plane_point;
        snap.plane_normal = w.plane_normal;
        return snap;
    };

    const SculptSnapshot sh = snapshot_of(hierarchy.workset());
    const SculptSnapshot sf = snapshot_of(fixed.workset());
    REQUIRE(sh.count == sf.count);

    std::vector<cfloat3> out_h(sh.count), out_f(sf.count);
    struct Named {
        const char* name;
        void (*run)(const SculptSnapshot&, const MeshBrushSettings&, cfloat3*);
    };
    const Named kernels[] = {
        {"grab", &mesh::kernel_grab},       {"pinch", &mesh::kernel_pinch},
        {"flatten", &mesh::kernel_flatten}, {"clay", &mesh::kernel_clay},
        {"crease", &mesh::kernel_crease},   {"nudge", &mesh::kernel_nudge},
    };
    for (const Named& k : kernels) {
        CAPTURE(k.name);
        k.run(sh, b, out_h.data());
        k.run(sf, b, out_f.data());
        for (std::size_t i = 0; i < sh.count; ++i) CHECK(same_bits(out_h[i], out_f[i]));
    }

    for (mesh::BrushFrame frame : {mesh::BrushFrame::RegionNormal, mesh::BrushFrame::VertexNormal}) {
        CAPTURE(static_cast<int>(frame));
        mesh::kernel_displace(sh, b, frame, out_h.data());
        mesh::kernel_displace(sf, b, frame, out_f.data());
        for (std::size_t i = 0; i < sh.count; ++i) CHECK(same_bits(out_h[i], out_f[i]));
    }
}

// -- P3: three representations, one vertex set --------------------------------

TEST_CASE("multires parity P3: all three representations write the same cage, bit for bit") {
    // THE THREE-WAY GATE, and the reason the fixture carries both quads and
    // triangles: ONE source model becomes a fixed mesh, an adaptive surface and
    // a hierarchy's cage, and a Grab — the only genuinely normal-free verb,
    // since `kernel_nudge` projects its direction against the vertex normal —
    // must land on the same bits in all three.
    //
    // If any one of them re-associated the weight, walked its region
    // differently, or dropped a zero-weight entry at a different point, this is
    // the assertion that moves.
    const Mesh source = plane_quads(8, 2.0f);
    const MeshBrushSettings b = cage_brush();

    // fixed
    Mesh fixed_mesh = source;
    MeshSculptor fixed(fixed_mesh, 0.0f);
    const std::size_t moved_fixed = fixed.stamp(MeshBrush::Grab, b);

    // adaptive
    auto surface = DynamicSurface::from_mesh(source);
    REQUIRE(surface.has_value());
    DynamicSculptor adaptive(*surface);
    const std::size_t moved_adaptive =
        adaptive.stamp(MeshBrush::Grab, b, topology_off()).moved_vertices;

    // the hierarchy, at its cage
    MultiresSurface s = build(source, 2);
    REQUIRE(s.set_sculpt_level(0));
    MultiresSculptor hierarchy(s);
    const std::size_t moved_hierarchy = hierarchy.stamp(MeshBrush::Grab, b);

    CHECK(moved_fixed == 9);
    CHECK(moved_adaptive == 9);
    CHECK(moved_hierarchy == 9);

    const std::vector<cfloat3>& cage = s.positions_at(0);
    REQUIRE(cage.size() == fixed_mesh.positions.size());

    std::vector<cfloat3> adaptive_positions;
    surface->vertices().for_each_live([&](VertexId, const DynamicVertex& v) {
        adaptive_positions.push_back(v.position);
    });
    REQUIRE(adaptive_positions.size() == fixed_mesh.positions.size());

    for (std::size_t i = 0; i < cage.size(); ++i) {
        CAPTURE(i);
        CHECK(same_bits(cage[i], fixed_mesh.positions[i]));
        CHECK(same_bits(adaptive_positions[i], fixed_mesh.positions[i]));
    }
}

TEST_CASE("multires parity P3: the automask reaches the hierarchy and masks the same set") {
    // The automask row, at the cage. `Boundary` and `TopologyConnected` are
    // set-valued and topological, so on the identical vertex set the hierarchy
    // and the fixed mesh must agree exactly — and the number must actually be
    // smaller than the unmasked one, or the case passes on an automask that did
    // nothing.
    const Mesh source = plane_quads(8, 2.0f);

    auto run = [&](std::uint32_t factors, std::vector<cfloat3>* out_cage) {
        MultiresSurface s = build(source, 1);
        REQUIRE(s.set_sculpt_level(0));
        Mesh flat = s.mesh_at_level(0, {/*normals=*/false, false, false});
        flat.normals = s.normals_at(0);
        MeshSculptor fixed(flat, 0.0f);
        MultiresSculptor hierarchy(s);

        MeshBrushSettings b = cage_brush();
        b.radius = 3.0f;  // reaches the patch's open border
        b.automask.factors = factors;

        const std::size_t h = hierarchy.stamp(MeshBrush::Grab, b);
        const std::size_t f = fixed.stamp(MeshBrush::Grab, b);
        CHECK(h == f);

        const std::vector<cfloat3>& cage = s.positions_at(0);
        REQUIRE(cage.size() == flat.positions.size());
        for (std::size_t i = 0; i < cage.size(); ++i) {
            CAPTURE(i);
            CHECK(same_bits(cage[i], flat.positions[i]));
        }
        *out_cage = cage;
        return h;
    };

    std::vector<cfloat3> open_cage, masked_cage;
    const std::size_t open = run(0, &open_cage);
    const std::size_t masked =
        run(AutomaskFactor::Boundary | AutomaskFactor::TopologyConnected, &masked_cage);

    CAPTURE(open);
    CAPTURE(masked);
    CHECK(open == 81);    // the whole 9x9 cage
    CHECK(masked == 49);  // the 7x7 interior: the border ring itself is the hard zero

    // WHY 49 AND NOT 25, WHICH `boundary_rings = 2` LOOKS LIKE IT SHOULD GIVE.
    // The fade RAMPS: a vertex on the border gets an exact zero and one a ring
    // inside gets a smoothstepped half, which is a smaller displacement and not
    // an absent one. A moved COUNT can only see the hard zero, so it reports
    // the same 49 at every ring setting — the ring count is visible in the
    // WEIGHTS, which is where the next case reads it. Asserting 25 here was the
    // first version of this test and it was measuring the wrong quantity.
    CHECK(masked < open);
}

// -- P4: above the cage, the difference is the frame round trip ---------------

TEST_CASE("multires parity P4: a fine stamp writes a coefficient where the fixed path writes a position") {
    // THE NAMED DIFFERENCE FOR THIS REPRESENTATION, and it is not a numeric
    // one. design.md is explicit that above level 0 there is no fixed mesh to
    // compare against — "P3 is a level-0 claim, and whether anything above it
    // is assertable beyond P1 and P2 is unresolved" — so inventing a tolerance
    // here would be inventing a claim. What IS the difference, and what a host
    // would notice first, is where the edit lands: the fixed sculptor writes
    // positions and the hierarchy writes DETAIL, leaving the cage exactly where
    // it was so that a coarse form change later still transports it.
    //
    // Both halves are asserted. A hierarchy that had started writing positions
    // through to the cage would pass every P1 and P2 case in this file.
    MultiresSurface s = build(plane_quads(8, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(2));

    const std::vector<cfloat3> cage_before = s.positions_at(0);
    const std::vector<cfloat3> level_before = s.positions_at(2);
    REQUIRE(s.detail_at(2).empty());

    Mesh flat = s.mesh_at_level(2, {/*normals=*/false, false, false});
    REQUIRE(flat.positions.size() == level_before.size());
    flat.normals = s.normals_at(2);
    MeshSculptor fixed(flat, 0.0f);
    MultiresSculptor hierarchy(s);

    MeshBrushSettings b = cage_brush();
    b.radius = 0.5f;

    const std::size_t moved_hierarchy = hierarchy.stamp(MeshBrush::Grab, b);
    const std::size_t moved_fixed = fixed.stamp(MeshBrush::Grab, b);
    CHECK(moved_hierarchy == moved_fixed);
    CHECK(moved_hierarchy == 45);

    // THE DIFFERENCE. The edit became detail...
    CHECK_FALSE(s.detail_at(2).empty());
    // ...and the cage two levels below it did not move by a byte, where the
    // fixed sculptor has no level below to leave alone.
    const std::vector<cfloat3>& cage_after = s.positions_at(0);
    REQUIRE(cage_after.size() == cage_before.size());
    for (std::size_t v = 0; v < cage_after.size(); ++v) {
        CAPTURE(v);
        CHECK(same_bits(cage_after[v], cage_before[v]));
    }

    // THE AGREEMENT, and it is stronger than the round trip the header warned
    // it might be. Measured on this flat cage: the level reconstructs to the
    // fixed sculptor's positions BIT FOR BIT for Grab, Clay, Draw, Inflate,
    // Nudge and Crease alike, because the transported frame over a planar cage
    // is axis-aligned and the encode/decode pair is exact there.
    //
    // So the tolerance is stated as what it is — an upper bound the measurement
    // sits far inside — rather than as a claim that the two differ. On a curved
    // cage they would differ, and this file does not assert a bound it has not
    // measured.
    const std::vector<cfloat3>& got = s.positions_at(2);
    REQUIRE(got.size() == flat.positions.size());
    std::size_t differing = 0;
    float worst = 0.0f;
    for (std::size_t v = 0; v < got.size(); ++v) {
        if (!same_bits(got[v], flat.positions[v])) ++differing;
        worst = std::max(worst, clength(got[v] - flat.positions[v]));
    }
    CAPTURE(differing);
    CAPTURE(worst);
    CHECK(differing == 0);
    CHECK(worst < 1e-5f);
}

TEST_CASE("multires parity: the boundary ring count is visible in the weights") {
    // THE HALF THE MOVED COUNT CANNOT SEE, and the reason the automask case
    // above reports the same 49 whatever `boundary_rings` says. The fade ramps
    // from zero at the border to one `rings` away; a wider setting therefore
    // does not mask MORE vertices, it masks the same ones and holds the ones
    // just inside them down harder.
    //
    // Read off the workset's own composed weights, which is where the automask
    // is a factor rather than an outcome.
    auto weights_for = [&](int rings) {
        MultiresSurface s = build(plane_quads(8, 2.0f), 1);
        REQUIRE(s.set_sculpt_level(0));
        MultiresSculptor hierarchy(s);

        MeshBrushSettings b = cage_brush();
        b.radius = 3.0f;
        b.falloff = mesh::MeshFalloff::Constant;  // so the only shaping is the automask
        b.automask.factors = static_cast<std::uint32_t>(AutomaskFactor::Boundary);
        b.automask.boundary_rings = rings;
        hierarchy.stamp(MeshBrush::Grab, b);

        // Keyed by level vertex, so two runs are comparable entry for entry
        // whatever order the walk produced.
        std::vector<float> out(81, -1.0f);
        const SculptWorkset& w = hierarchy.workset();
        for (std::size_t i = 0; i < w.size(); ++i) out[w.items[i].key()] = w.weights[i];
        return out;
    };

    const std::vector<float> one = weights_for(1);
    const std::vector<float> two = weights_for(2);

    // The cage is a 9x9 grid: index 0 is a corner, 10 is one ring in, 20 two.
    //
    // A BORDER VERTEX IS ABSENT FROM THE WORKSET, not present at weight zero,
    // and that is `compose_workset`'s second drop rather than an accident of
    // this fixture: a fully masked entry leaves the workset ENTIRELY, so the
    // stamp is bit-identical to its input there rather than merely close. The
    // sentinel is what says "the walk never handed this to a kernel".
    CHECK(one[0] == -1.0f);
    CHECK(two[0] == -1.0f);

    // ONE RING: everything off the border is at full weight.
    CHECK(one[10] == doctest::Approx(1.0f));
    // TWO RINGS: the same vertex is held at the smoothstepped half.
    CHECK(two[10] == doctest::Approx(0.5f));
    CHECK(two[10] < one[10]);
    // ...and two rings in, the wider setting has caught up.
    CHECK(two[20] == doctest::Approx(1.0f));
}

// -- the arena the hierarchy reports ------------------------------------------

TEST_CASE("multires parity: the reported arena is the bound level sculptor's") {
    // WHY THIS IS NOT A DETAIL. A multiresolution stamp IS the fixed
    // sculptor's stamp on the level's own mesh, so that is the arena that
    // grows; reporting a second, empty one here would tell a host budgeting
    // memory that a hierarchy's scratch cost nothing.
    MultiresSurface s = build(plane_quads(8, 2.0f), 2);
    MultiresSculptor hierarchy(s);

    // BEFORE THE FIRST STAMP there is no bound level and there is nothing to
    // report. Null rather than a zeroed struct, because "no level is bound" and
    // "the arena has spent nothing" are different facts.
    CHECK(hierarchy.arena() == nullptr);

    REQUIRE(s.set_sculpt_level(2));
    MeshBrushSettings b = cage_brush();
    b.radius = 0.5f;
    REQUIRE(hierarchy.stamp(MeshBrush::Grab, b) > 0);

    REQUIRE(hierarchy.arena() != nullptr);
    REQUIRE(hierarchy.level_sculptor() != nullptr);
    // THE SAME OBJECT, not a copy of its numbers.
    CHECK(hierarchy.arena() == &hierarchy.level_sculptor()->arena());
}

TEST_CASE("multires parity: an automasked stamp spends the arena, and then stops") {
    // The convergence claim on this representation. A stamp whose automask
    // needs no flood spends nothing at all — `NormalAngle` reads the workset's
    // own normals and `TopologyConnected` returns early on a region that is
    // already one component — so `Boundary` is the factor that makes the arena
    // do any work on a fixed-topology surface, and it is the one this uses.
    MultiresSurface s = build(plane_quads(8, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor hierarchy(s);

    MeshBrushSettings b = cage_brush();
    b.radius = 0.5f;
    b.strength = 0.05f;
    b.automask.factors = static_cast<std::uint32_t>(AutomaskFactor::Boundary);

    REQUIRE(hierarchy.stamp(MeshBrush::Grab, b) > 0);
    REQUIRE(hierarchy.arena() != nullptr);
    CHECK(hierarchy.arena()->growths() > 0);
    CHECK(hierarchy.arena()->high_water_bytes() > 0);

    for (int i = 0; i < 8; ++i) {
        b.center = cf3(0.0625f * static_cast<float>(i % 3), 0.0f, 0.0f);
        hierarchy.stamp(MeshBrush::Grab, b);
    }
    const std::size_t warm = hierarchy.arena()->growths();
    const std::size_t capacity = hierarchy.arena()->capacity_bytes();

    for (int i = 0; i < 32; ++i) {
        b.center = cf3(0.0625f * static_cast<float>(i % 3), 0.0f, 0.0f);
        hierarchy.stamp(MeshBrush::Grab, b);
    }

    CAPTURE(warm);
    CAPTURE(hierarchy.arena()->growths());
    CHECK(hierarchy.arena()->growths() == warm);
    CHECK(hierarchy.arena()->capacity_bytes() == capacity);
}

// -- the fourth consumer, which the merge of `main` introduced ----------------

// THE AUTOMASK REACHES `LayeredMultiresSculptor`, WHICH DID NOT EXIST WHEN THIS
// CHANGE WAS PLANNED.
//
// The change's headline claim is stated over three representations, and while
// it was being written `main` grew a FOURTH consumer of the runtime: the
// layered stroke transaction. It reaches the shared runtime by a route none of
// the other three take. `LayeredMultiresSculptor::gather` does not build a
// region of its own — it takes a zero-strength `Draw` through the level's own
// `MeshSculptor` and reads `workset()` back — precisely so the falloff, the
// mask gate, the alpha and the composed automask are the ONE answer the rest of
// the runtime gives rather than a second one drifting beside it.
//
// That route is load-bearing and nothing was watching it. The obvious
// "optimisation" for a region that only needs weights is a second walk that
// skips the automask composition entirely, and it would be invisible: `stamp`
// routes through `MultiresSculptor` and keeps its automask, so an artist would
// see masking work for the sixteen ordinary verbs and silently stop working for
// `erase`, `restore`, `smooth` and `stamp_detail` — the five verbs that exist
// only on this representation. The merge that brought the file in had to patch
// this walk to compile against the neutral `WorkItemId` array, which is a
// second reason to gate what it does rather than what it happens to build as.
//
// Asserted as a SET and not as a count. `erase` fades a coefficient by
// `weight * strength`, so the entry the automask removed is the one whose
// coefficient is EXACTLY what it was — which makes "what the eraser never
// reached" readable directly, and makes a wrong answer a named list of vertices
// rather than a number that is merely different. Note that this is the
// UNCHANGED set and not the non-zero one: away from the centre the eraser fades
// rather than clearing, so almost every vertex is still non-zero after a run
// that touched all 289 of them.
TEST_CASE("REGRESSION: the automask reaches the layered sculptor's own region walk") {
    const Mesh source = plane_quads(8, 2.0f);
    const LocalDetail pass{0.0f, 0.0f, 0.05f};

    std::vector<cfloat3> level1;
    auto run = [&](std::uint32_t factors, std::vector<std::uint32_t>* kept) {
        MultiresSurface s = build(source, 1);
        REQUIRE(s.set_sculpt_level(1));
        const SculptLayerId layer = s.add_sculpt_layer("pass");
        REQUIRE(layer != mesh::kNoSculptLayer);
        REQUIRE(s.set_active_sculpt_layer(layer));

        // A pass over EVERY level-1 vertex, so what the eraser leaves is what
        // the region left out rather than what was never there.
        const std::size_t n = s.positions_at(1).size();
        for (std::uint32_t v = 0; v < n; ++v)
            REQUIRE(s.set_sculpt_layer_detail(layer, 1, v, pass));

        LayeredMultiresSculptor sculpt(s);
        REQUIRE(sculpt.begin());
        MeshBrushSettings b;
        b.center = cf3(0, 0, 0);
        b.radius = 4.0f;  // past the far corner at 2.83, so nothing is out of reach
        b.strength = 1.0f;
        b.geodesic = false;
        b.automask.factors = factors;
        b.automask.boundary_rings = 2;
        const std::size_t moved = sculpt.erase(b);

        level1 = s.positions_at(1);
        kept->clear();
        for (std::uint32_t v = 0; v < n; ++v)
            if (s.sculpt_layer_detail(layer, 1, v) == pass) kept->push_back(v);
        return moved;
    };

    std::vector<std::uint32_t> kept_open, kept_masked;
    const std::size_t moved_open = run(0, &kept_open);
    const std::size_t moved_masked =
        run(static_cast<std::uint32_t>(AutomaskFactor::Boundary), &kept_masked);

    CAPTURE(moved_open);
    CAPTURE(moved_masked);
    CAPTURE(kept_open.size());
    CAPTURE(kept_masked.size());

    // THE UNMASKED RUN IS THE CONTROL, and it is the assertion that keeps the
    // masked one honest: a brush that reaches every vertex must leave nothing,
    // or "the border survived" would be a statement about the brush's reach.
    CHECK(kept_open.empty());
    CHECK(moved_open == 289);  // the whole 17x17 level-1 grid

    // AND THE MASKED ONE LEAVES EXACTLY THE OPEN BORDER. `boundary_rings = 2`
    // FADES over two rings and only the border itself reaches a hard zero — the
    // same reading `the boundary ring count is visible in the weights` records
    // for the cage — so the surviving set is the 64-vertex border ring and not
    // the 124 vertices two rings deep.
    CHECK(kept_masked.size() == 64);
    CHECK(moved_masked == 289 - 64);

    // AND IT IS THE BORDER, not merely a set of the right size. A region walk
    // that addressed the level by the wrong index would erase a scrambled 225
    // and leave a scrambled 64, which every count above would still accept.
    for (std::uint32_t v : kept_masked) {
        CAPTURE(v);
        REQUIRE(v < level1.size());
        const cfloat3 q = level1[v];
        CHECK((std::fabs(std::fabs(q.x) - 2.0f) < 1e-5f || std::fabs(std::fabs(q.z) - 2.0f) < 1e-5f));
    }
}
