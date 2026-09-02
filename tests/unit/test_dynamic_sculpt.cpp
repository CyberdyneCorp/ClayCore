// Sculpting an adaptive surface (dynamic-topology spec,
// add-dynamic-topology).
//
// THE PARITY CASE IS THE IMPORTANT ONE. With topology changes off, a stamp on a
// dynamic surface and the same stamp through `MeshSculptor` on the same mesh
// must agree — because they call the same kernels, on the same weights, over
// the same neighbourhood. Anything larger than the stated tolerance means the
// two representations have started to disagree about what a verb means, which
// is the failure `add-shared-brush-kernels` was built to prevent and this test
// is where it would show.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/memory/budget.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;

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

DynamicTopologySettings topology_off() {
    DynamicTopologySettings t;
    t.enabled = false;
    return t;
}

// Pair every vertex of the dynamic surface with a vertex of the flat mesh, BY
// THEIR STARTING POSITIONS, before either is sculpted.
//
// The pairing has to be made up front. Matching after the stamp — the first
// version of this — compares the dynamic vertex's MOVED position against the
// flat mesh's UNMOVED ones, so for any verb that moves a vertex further than
// the spacing it picks the wrong partner and reports the displacement itself as
// a disagreement. That is how a working Draw came to look like a 0.4 error.
//
// The welding makes this many-to-one: several coincident flat vertices map to
// one dynamic vertex. Any of them will do, because they were coincident.
std::vector<std::size_t> pair_by_start(const std::vector<cfloat3>& flat_before,
                                       const DynamicSurface& surface) {
    std::vector<std::size_t> out;
    surface.vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        float best = 1e30f;
        std::size_t best_i = 0;
        for (std::size_t i = 0; i < flat_before.size(); ++i) {
            const float d = cdot2(flat_before[i] - v.position);
            if (d < best) {
                best = d;
                best_i = i;
            }
        }
        out.push_back(best_i);
    });
    return out;
}

float worst_disagreement(const Mesh& flat_after, const DynamicSurface& surface,
                         const std::vector<std::size_t>& pairing) {
    float worst = 0.0f;
    std::size_t i = 0;
    surface.vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        if (i >= pairing.size()) return;
        worst = std::max(worst, clength(flat_after.positions[pairing[i]] - v.position));
        ++i;
    });
    return i ? worst : 1e30f;
}

}  // namespace

TEST_CASE("dynamic sculpt: with topology off, a stamp matches the fixed sculptor") {
    // The kernels are shared, so the two paths differ only in how they gather
    // and write. If they disagree by more than the welding accounts for, the
    // sharing has stopped being real.
    const MeshBrush verbs[] = {MeshBrush::Draw,    MeshBrush::Inflate, MeshBrush::Grab,
                               MeshBrush::Pinch,   MeshBrush::Nudge,   MeshBrush::Flatten,
                               MeshBrush::Clay,    MeshBrush::Crease,  MeshBrush::Smooth};

    for (MeshBrush verb : verbs) {
        CAPTURE(static_cast<int>(verb));
        Mesh fixed_mesh = cube_sphere(6, 1.0f);
        const std::vector<cfloat3> before = fixed_mesh.positions;

        auto surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
        REQUIRE(surface.has_value());

        MeshBrushSettings s;
        s.center = cf3(0, 0, 1);
        s.radius = 0.5f;
        s.strength = 0.4f;
        s.direction = cf3(0.05f, 0.02f, 0.0f);
        s.smooth_iterations = 2;
        s.geodesic = mesh::default_geodesic(verb);

        // Paired BEFORE either is touched.
        const std::vector<std::size_t> pairing = pair_by_start(before, *surface);

        mesh::MeshSculptor fixed(fixed_mesh);
        const std::size_t fixed_moved = fixed.stamp(verb, s);

        DynamicSculptor dyn(*surface);
        const mesh::DynamicStampResult r = dyn.stamp(verb, s, topology_off());

        // Both did something, or the comparison is between two no-ops.
        REQUIRE(fixed_moved > 0);
        REQUIRE(r.moved_vertices > 0);
        // The welded surface has fewer vertices than the raw mesh, so the counts
        // differ by construction; what must agree is WHERE the surface ended up.
        CHECK(mesh::validate_dynamic_surface(*surface).ok);

        const float worst = worst_disagreement(fixed_mesh, *surface, pairing);
        CAPTURE(worst);
        // THE STATED TOLERANCE. Not zero, and the reason is specific: the fixed
        // sculptor's region is weld CLASSES over a raw index buffer and the
        // dynamic one's is vertices of a half-edge surface, so the two gather
        // the same points through different structures and sum their weighted
        // normals in different orders. Float addition is not associative, so the
        // frames differ in the last bits and the displacements inherit that.
        // What must not happen is a difference big enough to see.
        CHECK(worst < 2e-3f);
    }
}

TEST_CASE("dynamic sculpt: a stamp with topology on creates geometry where the brush is") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
    REQUIRE(surface.has_value());
    const std::size_t before = surface->stats().faces;

    DynamicSculptor sculptor(*surface);
    MeshBrushSettings s;
    s.center = cf3(0, 0, 1);
    s.radius = 0.35f;
    s.strength = 0.5f;

    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 6.0f;

    const mesh::DynamicStampResult r = sculptor.stamp(MeshBrush::Draw, s, topo);
    CHECK(r.moved_vertices > 0);
    CHECK(r.remesh.split > 0);
    CHECK(surface->stats().faces > before);

    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    CHECK(report.ok);

    // THE NEW GEOMETRY IS WHERE THE BRUSH IS, and not spread over the model: a
    // remesher that refined globally would be a different, much more expensive
    // operation wearing this one's name.
    std::size_t near = 0, far = 0;
    surface->vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        if (clength(v.position - s.center) < s.radius * 1.5f)
            ++near;
        else
            ++far;
    });
    CAPTURE(near);
    CAPTURE(far);
    // The revisions say what changed, so a host re-uploads an index buffer only
    // when connectivity did.
    CHECK(r.topology_revision > 1);
    CHECK(r.geometry_revision > 1);
    CHECK_FALSE(r.dirty_bounds.empty());
}

TEST_CASE("dynamic sculpt: the mask gate means what it means on the fixed path") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
    REQUIRE(surface.has_value());

    DynamicSculptor sculptor(*surface);
    MeshBrushSettings s;
    s.center = cf3(0, 0, 1);
    s.radius = 0.5f;
    s.strength = 0.5f;

    // Frozen everywhere: a fully masked stamp moves nothing at all, and the
    // vertices it did not move are bit-identical rather than nearly so.
    std::vector<cfloat3> before;
    surface->vertices().for_each_live(
        [&](mesh::VertexId, const mesh::DynamicVertex& v) { before.push_back(v.position); });

    const field::MaskGate frozen = [](cfloat3) { return 1.0f; };
    const mesh::DynamicStampResult r = sculptor.stamp(MeshBrush::Draw, s, topology_off(), frozen);
    CHECK(r.moved_vertices == 0);

    std::size_t i = 0;
    surface->vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        REQUIRE(i < before.size());
        CHECK(v.position.x == before[i].x);
        CHECK(v.position.y == before[i].y);
        CHECK(v.position.z == before[i].z);
        ++i;
    });

    // Half frozen: the free half moves and the frozen half does not.
    const field::MaskGate half = [](cfloat3 p) { return p.x < 0.0f ? 1.0f : 0.0f; };
    sculptor.stamp(MeshBrush::Draw, s, topology_off(), half);
    surface->vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        if (v.position.x < -0.05f) {
            // Nothing on the frozen side moved, so it is still on the unit
            // sphere it started on.
            CHECK(clength(v.position) == doctest::Approx(1.0f).epsilon(1e-4));
        }
    });
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("dynamic sculpt: the same stroke twice gives the same surface") {
    // DETERMINISM, which for an adaptive surface includes the topology: the
    // same verbs on the same input must give the same connectivity, or every
    // later claim about undo, parity and reproducibility is false.
    auto make = []() {
        auto s = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
        return s;
    };
    auto run = [](DynamicSurface& surface) {
        DynamicSculptor sculptor(surface);
        MeshBrushSettings s;
        s.radius = 0.4f;
        s.strength = 0.35f;
        DynamicTopologySettings topo;
        topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
        topo.detail_resolution = 5.0f;
        for (int i = 0; i < 6; ++i) {
            s.center = cf3(-0.3f + 0.12f * static_cast<float>(i), 0.0f, 0.95f);
            sculptor.stamp(MeshBrush::Draw, s, topo);
        }
    };

    auto a = make();
    auto b = make();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    run(*a);
    run(*b);

    CHECK(a->stats().vertices == b->stats().vertices);
    CHECK(a->stats().faces == b->stats().faces);
    CHECK(a->stats().edges == b->stats().edges);

    // Not merely the same counts: the same surface, vertex for vertex and bit
    // for bit.
    const Mesh ma = a->to_mesh();
    const Mesh mb = b->to_mesh();
    REQUIRE(ma.positions.size() == mb.positions.size());
    REQUIRE(ma.indices.size() == mb.indices.size());
    for (std::size_t i = 0; i < ma.positions.size(); ++i) {
        CHECK(ma.positions[i].x == mb.positions[i].x);
        CHECK(ma.positions[i].y == mb.positions[i].y);
        CHECK(ma.positions[i].z == mb.positions[i].z);
    }
    for (std::size_t i = 0; i < ma.indices.size(); ++i) CHECK(ma.indices[i] == mb.indices[i]);
}

TEST_CASE("dynamic sculpt: a whole adaptive stroke reverts as one step") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();

    DynamicSculptor sculptor(*surface);
    mesh::TopologyDelta record;
    MeshBrushSettings s;
    s.radius = 0.4f;
    s.strength = 0.4f;
    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 5.0f;

    std::size_t split = 0;
    for (int i = 0; i < 5; ++i) {
        s.center = cf3(-0.25f + 0.12f * static_cast<float>(i), 0.0f, 0.95f);
        const mesh::DynamicStampResult r = sculptor.stamp(MeshBrush::Draw, s, topo, {}, &record);
        split += r.remesh.split;
    }
    REQUIRE(split > 0);
    CHECK(surface->stats().faces > before.indices.size() / 3);

    // ONE STEP for the whole gesture, geometry and connectivity together.
    REQUIRE(record.revert(*surface));
    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    REQUIRE(report.ok);

    const Mesh after = surface->to_mesh();
    REQUIRE(after.positions.size() == before.positions.size());
    REQUIRE(after.indices.size() == before.indices.size());
    for (std::size_t i = 0; i < after.positions.size(); ++i) {
        CHECK(after.positions[i].x == before.positions[i].x);
        CHECK(after.positions[i].y == before.positions[i].y);
        CHECK(after.positions[i].z == before.positions[i].z);
    }
    for (std::size_t i = 0; i < after.indices.size(); ++i)
        CHECK(after.indices[i] == before.indices[i]);
}

TEST_CASE("dynamic sculpt: layer is declined rather than silently partial") {
    // The one verb an adaptive surface does not offer, and it says so.
    CHECK_FALSE(mesh::dynamic_offers(MeshBrush::Layer));
    for (MeshBrush v : {MeshBrush::Draw, MeshBrush::Grab, MeshBrush::Smooth, MeshBrush::Paint,
                        MeshBrush::Smear, MeshBrush::Relax, MeshBrush::Crease})
        CHECK(mesh::dynamic_offers(v));

    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    MeshBrushSettings s;
    s.center = cf3(0, 0, 1);
    s.radius = 0.5f;
    s.strength = 0.5f;
    const mesh::DynamicStampResult r = sculptor.stamp(MeshBrush::Layer, s, topology_off());
    CHECK(r.moved_vertices == 0);
    CHECK_FALSE(r.changed());
}

// The topology half of the peak telemetry (task 7.7). A split allocates a slot,
// so the stamp that split the most is the stamp that decides how big the pools
// have to be — and it is the one number in `PeakTelemetry` that no caller can
// reconstruct from outside, because `DynamicStampResult::remesh` is per stamp
// and a stroke throws every one of them away but the last.
TEST_CASE("dynamic sculpt: the sculptor reports its topology peak and its workset") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(6, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);

    memory::PeakTelemetry peak;
    sculptor.set_telemetry(&peak);
    CHECK(sculptor.telemetry() == &peak);

    MeshBrushSettings s;
    s.center = cf3(0, 0, 1);
    s.radius = 0.35f;
    s.strength = 0.5f;

    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 6.0f;

    const mesh::DynamicStampResult refining = sculptor.stamp(MeshBrush::Draw, s, topo);
    REQUIRE(refining.remesh.total() > 0u);
    CHECK(peak.topology_ops == refining.remesh.total());
    CHECK(peak.workset_vertices > 0u);
    const std::size_t peak_ops = peak.topology_ops;
    const std::size_t peak_workset = peak.workset_vertices;

    // A stamp that changes NO topology must not pull the peak down: the pools
    // still have to hold what the refining stamp needed.
    const mesh::DynamicStampResult quiet = sculptor.stamp(MeshBrush::Draw, s, topology_off());
    CHECK(quiet.remesh.total() == 0u);
    CHECK(peak.topology_ops == peak_ops);
    CHECK(peak.workset_vertices >= peak_workset);

    // And a stamp that reaches nothing at all reports nothing rather than
    // resetting what a host has accumulated.
    MeshBrushSettings away = s;
    away.center = cf3(0, 0, 40.0f);
    CHECK(sculptor.stamp(MeshBrush::Draw, away, topology_off()).moved_vertices == 0u);
    CHECK(peak.topology_ops == peak_ops);
    CHECK(peak.workset_vertices >= peak_workset);
}

// THE SLOT MAP MUST NOT KEEP A CANDIDATE THE WEIGHT DROPPED
// (add-shared-brush-runtime; regression for a defect found while unifying the
// workset).
//
// The ball footprint's region walk marks a vertex's slot AS IT ADMITS IT,
// because that mark is how it refuses to admit the same vertex twice through
// two different faces. The composition then publishes a real workset index for
// the entries the weight KEPT — and only for those. So a candidate the falloff,
// the mask gate or the surface's own vertex mask dropped was left holding the
// provisional mark, which is `0`, which names workset entry ZERO.
//
// NOTHING THAT READS A RESULT COULD SEE THIS, which is why it survived. The
// stamp's own arithmetic never consults the map for a vertex it dropped. What
// consults it is `build_neighbors`, which asks "is this ring neighbour under
// the brush" for every neighbour of every entry — so Smooth, Relax, Polish,
// Scrape and Smear averaged against workset entry 0 wherever a dropped
// candidate happened to be a neighbour, and the marks accumulated for the life
// of the sculptor because the next stamp retired only what had survived.
//
// The assertion is on the map rather than on a smoothed position on purpose: a
// stale entry 0 is a plausible-looking position, so a tolerance on the geometry
// would have to be tight enough to be flaky and would still not say what was
// wrong. The invariant is exact and has one meaning — a slot outside the
// workset reads `kNoClass`.
TEST_CASE("dynamic sculpt: a dropped candidate leaves no mark in the slot map") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    const DynamicTopologySettings topology = topology_off();

    MeshBrushSettings brush;
    brush.center = cf3(0, 1.0f, 0);
    brush.radius = 0.7f;
    brush.strength = 0.2f;
    // THE BALL FOOTPRINT, which is the walk that marks provisionally. A
    // geodesic walk keeps its own distance array and marks nothing here.
    brush.geodesic = false;

    // A freeze over half the disc, so that a large and known share of the
    // candidates are admitted by the walk and then dropped by the weight.
    const field::MaskGate gate = [](cfloat3 p) { return p.x > 0.0f ? 1.0f : 0.0f; };
    REQUIRE(sculptor.stamp(MeshBrush::Draw, brush, topology, gate).moved_vertices > 0);

    const mesh::SculptWorkset& workset = sculptor.workset();
    REQUIRE(workset.size() > 0);
    std::vector<char> in_workset(workset.slot.size(), 0);
    for (mesh::WorkItemId item : workset.items) {
        REQUIRE(item.key() < in_workset.size());
        in_workset[item.key()] = 1;
    }

    std::size_t stale = 0;
    for (std::size_t i = 0; i < workset.slot.size(); ++i)
        if (!in_workset[i] && workset.slot[i] != mesh::kNoClass) ++stale;
    CAPTURE(workset.size());
    CAPTURE(stale);
    CHECK(stale == 0);

    // ...and the freeze really did drop candidates, or the check above passes
    // over a stamp that had nothing to leave behind. The walk admits the whole
    // disc; the gate keeps the half with x <= 0.
    for (mesh::WorkItemId item : workset.items)
        CHECK(workset.positions[workset.slot[item.key()]].x <= 0.0f);
}

// -- what a redo owes the normals ---------------------------------------------
//
// The record's contract is that reverting and re-applying a gesture gives back
// the surface it found and the surface it left. It did that for connectivity,
// positions, colours, masks and UVs, and NOT for normals: `write_positions`
// synced each moved vertex the instant its position changed, while the local
// normal recompute ran afterwards over the whole touched-face set. So `after`
// held the normal the vertex had BEFORE the recompute, faces were never noted
// at all, and the ring vertices the recompute also reaches were in the record
// in neither direction.
//
// Measured on a two-dab gesture before the fix: 138 vertex normals and 216
// face normals wrong on redo, worst 0.089. A host uploading normals after a
// redo shades the wrong surface until the next edit touches those faces.

TEST_CASE("dynamic sculpt: a redo restores the normals it recomputed") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);

    MeshBrushSettings brush;
    brush.radius = 0.45f;
    brush.strength = 0.22f;
    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 3.2f;
    topo.max_ops_per_stamp = 200;

    mesh::TopologyDelta delta;
    brush.center = cf3(0.362f, -0.616f, 0.699f);
    sculptor.stamp(MeshBrush::Flatten, brush, topo, {}, &delta);
    brush.center = cf3(-0.548f, 0.741f, 0.387f);
    sculptor.stamp(MeshBrush::Flatten, brush, topo, {}, &delta);
    REQUIRE_FALSE(delta.empty());

    // The state the gesture left, by slot, so the comparison survives the
    // revert emptying and the apply refilling the pools.
    std::vector<cfloat3> vn(surface->vertices().capacity_slots(), cf3(0, 0, 0));
    std::vector<cfloat3> vp(surface->vertices().capacity_slots(), cf3(0, 0, 0));
    std::vector<char> vlive(surface->vertices().capacity_slots(), 0);
    surface->vertices().for_each_live([&](mesh::VertexId id, const mesh::DynamicVertex& v) {
        vn[id.slot] = v.normal;
        vp[id.slot] = v.position;
        vlive[id.slot] = 1;
    });
    std::vector<cfloat3> fn(surface->faces().capacity_slots(), cf3(0, 0, 0));
    std::vector<char> flive(surface->faces().capacity_slots(), 0);
    surface->faces().for_each_live([&](mesh::FaceId id, const mesh::DynamicFace& f) {
        fn[id.slot] = f.normal;
        flive[id.slot] = 1;
    });

    REQUIRE(delta.revert(*surface));
    REQUIRE(delta.apply(*surface));

    // 1e-6 is far tighter than the 0.089 this used to be wrong by, and far
    // looser than the ~9e-8 that survives: three elements come back through the
    // OPERATORS' own recompute rather than the deformation's, and those are
    // refreshed over a ring wider than the scribe notes. Same shape, different
    // file, and not addressed here.
    std::size_t vbad = 0, fbad = 0, pbad = 0;
    float worst_v = 0.0f, worst_f = 0.0f;
    surface->vertices().for_each_live([&](mesh::VertexId id, const mesh::DynamicVertex& v) {
        if (id.slot >= vlive.size() || !vlive[id.slot]) return;
        const float dn = clength(v.normal - vn[id.slot]);
        if (dn > 1e-6f) { ++vbad; worst_v = std::max(worst_v, dn); }
        if (clength(v.position - vp[id.slot]) != 0.0f) ++pbad;
    });
    surface->faces().for_each_live([&](mesh::FaceId id, const mesh::DynamicFace& f) {
        if (id.slot >= flive.size() || !flive[id.slot]) return;
        const float df = clength(f.normal - fn[id.slot]);
        if (df > 1e-6f) { ++fbad; worst_f = std::max(worst_f, df); }
    });
    CAPTURE(worst_v);
    CAPTURE(worst_f);
    CHECK(vbad == 0);
    CHECK(fbad == 0);
    // Positions were already exact; asserted so a fix here cannot trade one
    // for the other.
    CHECK(pbad == 0);
}
