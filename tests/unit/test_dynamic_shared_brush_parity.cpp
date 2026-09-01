// THE SHARED BRUSH RUNTIME, AGAINST THE ADAPTIVE SURFACE (meshing and
// brush-engine specs, add-shared-brush-runtime 6.4).
//
// `test_mesh_sculpt_parity.cpp` pins the FIXED path to its own bits and is the
// acceptance gate this whole change is measured against. It says nothing about
// the other two representations, and that silence is where the divergence this
// change exists to close lived unnoticed: `DynamicSculptor::gather` was handed
// `brush.automask` on every stamp and read none of it, so an automask an artist
// enabled was silently absent on the adaptive surface while
// `clay_dynamic_sculptor_stamp`'s own header promised "the same descriptor the
// fixed path takes".
//
// WHAT PARITY CAN AND CANNOT MEAN HERE — design.md D8, in four rows.
//
//   P1  the kernels. Given an identical snapshot, every kernel writes
//       byte-identical displacements whoever built it.
//   P2  the weight. One `compose_weight`, in one factor order, on every
//       representation.
//   P3  an identical vertex set and an identical stamp. On a plane grid
//       converted with `DynamicSurface::from_mesh` the two representations hold
//       the same vertices in the same order, so a verb that reads no normal
//       must write byte-identical positions. This is the strong gate.
//   P4  the named differences, asserted AS differences. On a CURVED surface the
//       two normal estimators disagree — `class_normal` is angle-weighted and
//       reaches `acos`, `DynamicSurface`'s averages the cached face normals —
//       so Draw MUST differ. A gate that only checked agreement would be
//       satisfied by two representations that had become equally wrong, and a
//       divergence that suddenly vanished would mean an estimator had been
//       silently unified, which is a real change to what a brush does.
//
// EVERY FIXTURE COORDINATE IS A POWER OF TWO OR A SUM OF THEM, for the reason
// `test_voxel_mesh_fixture.cpp` recorded: the fixture must not be a second
// source of rounding the byte comparisons would then be measuring.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/sculpt_kernels.h"

using namespace clay;
using namespace clay::kernel;
using mesh::AutomaskFactor;
using mesh::DynamicSculptor;
using mesh::DynamicStampResult;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::DynamicVertex;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;
using mesh::SculptSnapshot;
using mesh::SculptWorkset;
using mesh::VertexId;

namespace {

bool same_bits(cfloat3 a, cfloat3 b) { return std::memcmp(&a, &b, sizeof(cfloat3)) == 0; }

// RIPPLED, for the reason `test_mesh_sculpt_parity.cpp` gives: on a perfectly
// flat grid the Laplacian of every interior vertex is the vertex itself, so
// half the verbs contribute a parity claim that pins a no-op. The heights come
// from an exact eighth-scale table.
float ripple_height(int x, int z) {
    static const float kWave[8] = {0.0f,     0.0625f,  0.125f,  0.0625f,
                                   0.0f,     -0.0625f, -0.125f, -0.0625f};
    return kWave[(x + z) & 7];
}

// THE P3 FIXTURE. No duplicated vertices, so the fixed mesh's weld classes, the
// adaptive surface's vertex slots and the input indices are one and the same
// numbering — which is what makes "the same vertices in the same order" a fact
// rather than a pairing heuristic. It is asserted below rather than assumed.
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

// THE P4 FIXTURE: curvature, so the two normal estimators have something to
// disagree about. A cube-sphere rather than a UV sphere, because normalizing a
// cube grid uses only multiply, add, divide and sqrt — all correctly rounded —
// where sin and cos are not.
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

// The surface's live vertices, in slot order.
std::vector<cfloat3> positions_of(const DynamicSurface& surface) {
    std::vector<cfloat3> out;
    surface.vertices().for_each_live(
        [&](VertexId, const DynamicVertex& v) { out.push_back(v.position); });
    return out;
}

std::vector<VertexId> ids_of(const DynamicSurface& surface) {
    std::vector<VertexId> out;
    surface.vertices().for_each_live([&](VertexId id, const DynamicVertex&) { out.push_back(id); });
    return out;
}

MeshBrushSettings euclidean_brush() {
    MeshBrushSettings b;
    b.center = cf3(0, 0, 0);
    b.radius = 0.5f;
    b.strength = 0.5f;
    // EUCLIDEAN. The two geodesic walks are a weld-class ring and a half-edge
    // ring; they reach the same vertices on this fixture but their path
    // distances are separate accumulations, so the taper is one more thing that
    // could differ at the last bit. P3 is about the composition, not the walk.
    b.geodesic = false;
    b.direction = cf3(0.125f, 0.25f, 0.0f);
    return b;
}

}  // namespace

// -- the fixture's own precondition -------------------------------------------

TEST_CASE("adaptive parity: the fixture really is one vertex set in one order") {
    // ASSERTED, NOT ASSUMED. Every byte comparison below indexes the fixed
    // mesh by the adaptive vertex's slot, and that is only meaningful because
    // `from_mesh` on a grid with no duplicates keeps the input numbering. If a
    // future welding change broke it, the parity cases would start comparing
    // unrelated vertices and would fail for a reason that had nothing to do
    // with the brush.
    const Mesh base = plane_grid(16, 1.0f);
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());

    const std::vector<VertexId> ids = ids_of(*surface);
    const std::vector<cfloat3> pos = positions_of(*surface);
    REQUIRE(ids.size() == base.positions.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(ids[i].slot == static_cast<std::uint32_t>(i));
        CHECK(same_bits(pos[i], base.positions[i]));
    }
}

// -- P1 and P2: the workset the two adapters hand the kernels ------------------

TEST_CASE("adaptive parity P1/P2: the two adapters build the same workset, bit for bit") {
    // THE STRONGEST FORM OF P1 AND P2 AVAILABLE. Rather than building a
    // synthetic snapshot and feeding it to both — which would prove only that
    // one function is one function — this compares the worksets the two
    // representations' OWN walks produced from the same stamp, entry by entry.
    //
    // Every field here is one the kernels read. If the adaptive path ever
    // re-derived a factor instead of calling `compose_workset`, or applied the
    // five factors in a different order, or resolved the stamp's frame from
    // what it was about to deposit, it would show up in this one comparison
    // before it showed up in any position.
    const Mesh base = plane_grid(16, 1.0f);

    Mesh fixed_mesh = base;
    MeshSculptor fixed(fixed_mesh, 0.0f);
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    DynamicSculptor adaptive(*surface);

    const MeshBrushSettings b = euclidean_brush();
    REQUIRE(fixed.stamp(MeshBrush::Grab, b) > 0);
    REQUIRE(adaptive.stamp(MeshBrush::Grab, b, topology_off()).moved_vertices > 0);

    const SculptWorkset& a = fixed.workset();
    const SculptWorkset& d = adaptive.workset();

    REQUIRE(a.size() == d.size());
    REQUIRE(a.size() > 20);  // a workset of two entries would pass vacuously

    for (std::size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        // THE DENSE HALF, which is the one thing the two identities share:
        // a weld class on one side, a vertex slot on the other, and on this
        // fixture they are the same number. `WorkItemId::key()` is the neutral
        // spelling of exactly that.
        CHECK(a.items[i].key() == d.items[i].key());
        CHECK(same_bits(a.positions[i], d.positions[i]));
        CHECK(same_bits(a.normals[i], d.normals[i]));
        // P2, per entry: the composed weight, to the bit.
        CHECK(std::memcmp(&a.weights[i], &d.weights[i], sizeof(float)) == 0);
    }

    // The stamp's frame, resolved from the snapshot by the shared composition.
    CHECK(same_bits(a.average_normal, d.average_normal));
    CHECK(same_bits(a.centroid, d.centroid));
    CHECK(same_bits(a.plane_point, d.plane_point));
    CHECK(same_bits(a.plane_normal, d.plane_normal));

    // And the WRITE REGION — the subset that actually moved — agrees too, which
    // is the half a host uploads.
    REQUIRE(a.write_region.size() == d.write_region.size());
    for (std::size_t i = 0; i < a.write_region.size(); ++i)
        CHECK(a.write_region[i].key() == d.write_region[i].key());
}

TEST_CASE("adaptive parity P1: every kernel agrees on a snapshot each adapter built") {
    // P1 proper. The two worksets above are byte-identical, so a kernel run
    // over each must write byte-identical displacements — and running them is
    // what says the SNAPSHOT is the whole of what a kernel reads, with nothing
    // reaching around it into a representation.
    const Mesh base = plane_grid(16, 1.0f);

    Mesh fixed_mesh = base;
    MeshSculptor fixed(fixed_mesh, 0.0f);
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    DynamicSculptor adaptive(*surface);

    MeshBrushSettings b = euclidean_brush();
    b.deposit_normal = cf3(0, 0, 0);
    REQUIRE(fixed.stamp(MeshBrush::Grab, b) > 0);
    REQUIRE(adaptive.stamp(MeshBrush::Grab, b, topology_off()).moved_vertices > 0);

    auto snapshot_of = [](const SculptWorkset& w) {
        SculptSnapshot s;
        s.positions = w.positions.data();
        s.normals = w.normals.data();
        s.weights = w.weights.data();
        s.count = w.size();
        s.average_normal = w.average_normal;
        s.centroid = w.centroid;
        s.plane_point = w.plane_point;
        s.plane_normal = w.plane_normal;
        return s;
    };

    const SculptSnapshot sa = snapshot_of(fixed.workset());
    const SculptSnapshot sd = snapshot_of(adaptive.workset());
    REQUIRE(sa.count == sd.count);

    std::vector<cfloat3> out_a(sa.count), out_d(sd.count);

    // The neighbour-free kernels, which is every verb whose displacement is a
    // function of the snapshot alone. The smoothing family needs a
    // `SculptNeighbors`, which is genuinely per-representation storage and is
    // covered by the position comparison in P3 instead.
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
        k.run(sa, b, out_a.data());
        k.run(sd, b, out_d.data());
        for (std::size_t i = 0; i < sa.count; ++i) {
            CAPTURE(i);
            CHECK(same_bits(out_a[i], out_d[i]));
        }
    }

    // Draw and Inflate are the same kernel under two frames, and both are
    // included because the frame is read from the SNAPSHOT — a sculptor that
    // left `average_normal` unset would produce a plausible-looking result here
    // and only here.
    for (mesh::BrushFrame frame : {mesh::BrushFrame::RegionNormal, mesh::BrushFrame::VertexNormal}) {
        CAPTURE(static_cast<int>(frame));
        mesh::kernel_displace(sa, b, frame, out_a.data());
        mesh::kernel_displace(sd, b, frame, out_d.data());
        for (std::size_t i = 0; i < sa.count; ++i) CHECK(same_bits(out_a[i], out_d[i]));
    }
}

TEST_CASE("adaptive parity P2: the factor order is the contract, and it is not associative") {
    // WHAT MAKES THE PER-ENTRY WEIGHT COMPARISON ABOVE A GATE. If the five
    // multiplications could be done in any order, "one `compose_weight`" would
    // be a tidiness argument rather than a correctness one and a
    // representation that inlined its own product would agree anyway.
    //
    // They cannot. Float multiplication is not associative, and on this named
    // factor set the documented order and a re-associated one differ in the
    // last bit — as do 16312 of the 32768 combinations drawn from the same
    // eight values.
    //
    // AND THIS CASE IS THE ONLY GATE ON IT, WHICH IS MEASURED. Reversing the
    // five multiplications in `compose_weight` and rebuilding leaves every
    // golden in `mesh_sculpt_goldens_*.inc` passing and every parity comparison
    // in this file passing: those stamps carry a zero gate, no alpha and no
    // automask, so four of the five factors are an exact 1 and the product is
    // order-independent for them. The order only becomes observable when a
    // stroke actually uses the gate, the alpha and the automask together —
    // which is what a real stroke does and what no fixture in the suite
    // reaches. So this is not a belt-and-braces assertion on top of the
    // goldens; it is the assertion.
    mesh::WeightFactors f;
    f.falloff = 0.1f;
    f.path_taper = 0.1f;
    f.gate = 0.1f;
    f.alpha = 0.1f;
    f.automask = 0.9f;

    const float ordered = mesh::compose_weight(f);

    // The same five numbers, folded the other way round.
    const float reassociated = ((f.automask * f.alpha) * (1.0f - f.gate)) * f.path_taper * f.falloff;
    CHECK(std::memcmp(&ordered, &reassociated, sizeof(float)) != 0);
    CHECK(ordered == doctest::Approx(reassociated));  // ...and only in the last bit

    // The order itself, spelled out. This is the expression `compose_weight`
    // must be; a change to either makes the two disagree.
    float w = f.falloff;
    w *= f.path_taper;
    w *= 1.0f - std::clamp(f.gate, 0.0f, 1.0f);
    w *= f.alpha;
    w *= f.automask;
    CHECK(std::memcmp(&ordered, &w, sizeof(float)) == 0);

    // AND THE AUTOMASK IS LAST, WITH AN IDENTITY OF EXACTLY ONE, which is what
    // makes a stamp with no automask bit-identical to one from before
    // automasking existed rather than merely close. Multiplying a 1.0 in at the
    // END is a no-op on every float; multiplying it in EARLIER is not, because
    // it would move the four remaining products into a different association.
    mesh::WeightFactors unmasked = f;
    unmasked.automask = 1.0f;
    const float with_identity = mesh::compose_weight(unmasked);

    float without = f.falloff;
    without *= f.path_taper;
    without *= 1.0f - std::clamp(f.gate, 0.0f, 1.0f);
    without *= f.alpha;
    CHECK(std::memcmp(&with_identity, &without, sizeof(float)) == 0);
}

// -- P3: an identical vertex set, an identical stamp --------------------------

TEST_CASE("adaptive parity P3: a normal-free verb writes byte-identical positions") {
    // THE STRONG GATE. `kernel_grab` reads `settings.direction` and
    // `weights[i]` and nothing else, so every difference the two
    // representations could have — the walk, the drop rule, the factor order,
    // the frame resolution — lands in the weight and shows up here as a byte.
    const Mesh base = plane_grid(16, 1.0f);

    Mesh fixed_mesh = base;
    MeshSculptor fixed(fixed_mesh, 0.0f);
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    DynamicSculptor adaptive(*surface);

    const MeshBrushSettings b = euclidean_brush();
    const std::size_t moved_fixed = fixed.stamp(MeshBrush::Grab, b);
    const DynamicStampResult r = adaptive.stamp(MeshBrush::Grab, b, topology_off());

    CHECK(moved_fixed == r.moved_vertices);
    CHECK(moved_fixed == 45);  // the measured footprint; a stamp that reached nothing would pass

    const std::vector<cfloat3> got = positions_of(*surface);
    REQUIRE(got.size() == fixed_mesh.positions.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (!same_bits(got[i], fixed_mesh.positions[i])) ++differing;
    CHECK(differing == 0);
}

TEST_CASE("adaptive parity P3: Nudge agrees where the two estimators agree exactly") {
    // NUDGE IS NORMAL-FREE IN ITS DIRECTION AND NOT IN ITS PROJECTION —
    // `kernel_nudge` is `tangential(direction, normals[i])`. So it is a P3 verb
    // on this fixture and NOT in general, and saying which is which is the
    // point of separating it from the Grab case above: on the curved fixture
    // below the same verb disagrees at two vertices, which is P4 behaviour and
    // correct.
    //
    // On the plane grid the two estimators produce byte-identical normals — the
    // fixture assertion in the P1/P2 case is what says so — so the projection
    // is the same projection and the positions must match.
    const Mesh base = plane_grid(16, 1.0f);

    Mesh fixed_mesh = base;
    MeshSculptor fixed(fixed_mesh, 0.0f);
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    DynamicSculptor adaptive(*surface);

    const MeshBrushSettings b = euclidean_brush();
    REQUIRE(fixed.stamp(MeshBrush::Nudge, b) == 45);
    REQUIRE(adaptive.stamp(MeshBrush::Nudge, b, topology_off()).moved_vertices == 45);

    const std::vector<cfloat3> got = positions_of(*surface);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(same_bits(got[i], fixed_mesh.positions[i]));
    }
}

// -- P3, the automask row -----------------------------------------------------

TEST_CASE("adaptive parity P3: a topological automask reaches the same vertex set") {
    // THE ROW THAT PROVES THE AUTOMASK ACTUALLY REACHES THE ADAPTIVE PATH,
    // rather than being present and returning ones. `Boundary` and
    // `TopologyConnected` are set-valued and purely topological — they ask
    // `WorkItemTopology` the two questions and nothing else — so on an
    // identical vertex set they must agree EXACTLY, not approximately.
    //
    // The radius is large enough to reach the grid's open border, which is what
    // gives the boundary fade something to do; a brush in the middle of the
    // patch would make this pass by masking nothing.
    const Mesh base = plane_grid(16, 1.0f);

    auto stamp_both = [&](std::uint32_t factors, std::size_t* out_fixed, std::size_t* out_adaptive,
                          std::vector<cfloat3>* out_positions) {
        Mesh fixed_mesh = base;
        MeshSculptor fixed(fixed_mesh, 0.0f);
        auto surface = DynamicSurface::from_mesh(base);
        REQUIRE(surface.has_value());
        DynamicSculptor adaptive(*surface);

        MeshBrushSettings b = euclidean_brush();
        b.radius = 1.5f;
        b.automask.factors = factors;

        *out_fixed = fixed.stamp(MeshBrush::Grab, b);
        *out_adaptive = adaptive.stamp(MeshBrush::Grab, b, topology_off()).moved_vertices;
        *out_positions = positions_of(*surface);

        // The byte comparison, which is the assertion the counts alone could
        // not make: two representations could mask the same NUMBER of vertices
        // and mask different ones.
        REQUIRE(out_positions->size() == fixed_mesh.positions.size());
        for (std::size_t i = 0; i < out_positions->size(); ++i) {
            CAPTURE(i);
            CHECK(same_bits((*out_positions)[i], fixed_mesh.positions[i]));
        }
    };

    std::size_t fixed_open = 0, adaptive_open = 0;
    std::vector<cfloat3> open_positions;
    stamp_both(0, &fixed_open, &adaptive_open, &open_positions);

    std::size_t fixed_masked = 0, adaptive_masked = 0;
    std::vector<cfloat3> masked_positions;
    stamp_both(AutomaskFactor::Boundary | AutomaskFactor::TopologyConnected, &fixed_masked,
               &adaptive_masked, &masked_positions);

    CHECK(fixed_open == adaptive_open);
    CHECK(fixed_masked == adaptive_masked);

    // The measured numbers, so a change that made both representations equally
    // wrong is still visible: this brush reaches all 289 vertices of the 17x17
    // patch and 225 — the 15x15 interior — survive the automask.
    //
    // 225 AND NOT 169, WHICH `boundary_rings = 2` LOOKS LIKE IT SHOULD LEAVE.
    // The fade RAMPS: only the border ring itself reaches an exact zero, and a
    // vertex one ring in is held at a smoothstepped half, which is a smaller
    // displacement rather than an absent one. The ring count is visible in the
    // WEIGHTS and not in a moved count — `test_multires_shared_brush_parity.cpp`
    // reads it there.
    CHECK(fixed_open == 289);
    CHECK(fixed_masked == 225);

    // AND THE AUTOMASK ACTUALLY DID SOMETHING. Without this the case above is
    // satisfied by an automask that masked nothing on either representation,
    // which is exactly the state this change found the adaptive path in.
    CHECK(adaptive_masked < adaptive_open);
}

// -- 4.5: the regression this change exists to close --------------------------

TEST_CASE("REGRESSION: the automask an artist enables reaches the adaptive surface") {
    // THE DIVERGENCE, AS A TEST. Before this change `DynamicSculptor::gather`
    // never read `brush.automask` at all, so this stamp moved 365 vertices with
    // the factor set and 365 without it. It now moves 149 with, and the fixed
    // path moves the same 149 from the same argument.
    //
    // NORMAL ANGLE, tightened to 0.5 radians so that the gate actually CLOSES
    // rather than merely fading: the factor is full strength up to the angle
    // and zero at twice it, and at the 60-degree default nothing on a unit
    // sphere under a 1.6 brush turns far enough away to reach zero — the first
    // version of this test set no angle and reported 365 against 365 for a
    // reason that had nothing to do with the defect.
    const Mesh base = cube_sphere(10, 1.0f);

    auto stamp = [&](std::uint32_t factors, std::vector<cfloat3>* out_before,
                     std::vector<cfloat3>* out_after) {
        auto surface = DynamicSurface::from_mesh(base);
        REQUIRE(surface.has_value());
        *out_before = positions_of(*surface);
        DynamicSculptor adaptive(*surface);

        MeshBrushSettings b;
        b.center = cf3(0, 0, 1.0f);
        b.radius = 1.6f;
        b.strength = 0.4f;
        b.geodesic = false;
        b.direction = cf3(0.0f, 0.0f, 0.125f);
        b.automask.factors = factors;
        b.automask.normal_angle = 0.5f;

        const DynamicStampResult r = adaptive.stamp(MeshBrush::Grab, b, topology_off());
        *out_after = positions_of(*surface);
        return r.moved_vertices;
    };

    std::vector<cfloat3> before_open, after_open;
    const std::size_t open = stamp(0, &before_open, &after_open);

    std::vector<cfloat3> before_masked, after_masked;
    const std::size_t masked = stamp(static_cast<std::uint32_t>(AutomaskFactor::NormalAngle),
                                    &before_masked, &after_masked);

    CAPTURE(open);
    CAPTURE(masked);
    CHECK(open == 365);
    CHECK(masked == 149);
    CHECK(masked < open);

    // AND THE DIFFERENCE IS ON THE SIDE FACING AWAY, which is the half a count
    // cannot say. The brush faces +Z; every vertex that survived the factor
    // must face within twice the angle of it, and the ones it removed must not.
    // `cos(2 * 0.5) = 0.5403`.
    float worst_survivor = 1.0f;
    for (std::size_t i = 0; i < before_masked.size(); ++i) {
        if (same_bits(after_masked[i], before_masked[i])) continue;
        const cfloat3 facing = before_masked[i] / clength(before_masked[i]);
        worst_survivor = std::min(worst_survivor, cdot(facing, cf3(0, 0, 1)));
    }
    CAPTURE(worst_survivor);
    CHECK(worst_survivor > 0.5403f);

    // ...and without the factor the stamp reached well round the far side, so
    // the bound above is a real constraint rather than one the footprint met
    // anyway.
    float worst_open = 1.0f;
    for (std::size_t i = 0; i < before_open.size(); ++i) {
        if (same_bits(after_open[i], before_open[i])) continue;
        const cfloat3 facing = before_open[i] / clength(before_open[i]);
        worst_open = std::min(worst_open, cdot(facing, cf3(0, 0, 1)));
    }
    CAPTURE(worst_open);
    CHECK(worst_open < 0.0f);
}

TEST_CASE("REGRESSION: the adaptive automask agrees with the fixed one it was missing") {
    // The companion claim, and the one the C ABI's header actually promises:
    // "the same descriptor the fixed path takes". Same fixture, same argument,
    // same two numbers on both representations.
    const Mesh base = cube_sphere(10, 1.0f);

    MeshBrushSettings b;
    b.center = cf3(0, 0, 1.0f);
    b.radius = 1.6f;
    b.strength = 0.4f;
    b.geodesic = false;
    b.direction = cf3(0.0f, 0.0f, 0.125f);
    b.automask.normal_angle = 0.5f;

    auto both = [&](std::uint32_t factors) {
        MeshBrushSettings s = b;
        s.automask.factors = factors;

        Mesh fixed_mesh = base;
        MeshSculptor fixed(fixed_mesh, 1e-5f);
        auto surface = DynamicSurface::from_mesh(base);
        REQUIRE(surface.has_value());
        DynamicSculptor adaptive(*surface);

        const std::size_t f = fixed.stamp(MeshBrush::Grab, s);
        const std::size_t d = adaptive.stamp(MeshBrush::Grab, s, topology_off()).moved_vertices;
        CHECK(f == d);
        return f;
    };

    CHECK(both(0) == 365);
    CHECK(both(static_cast<std::uint32_t>(AutomaskFactor::NormalAngle)) == 149);
}

// -- P4: the named differences, asserted AS differences ------------------------

TEST_CASE("adaptive parity P4: Draw differs between the representations, and by how much") {
    // A GATE ON THE DIVERGENCE ITSELF. `class_normal` is angle-weighted and
    // reaches `acos`; `DynamicSurface::compute_vertex_normal` averages the
    // cached face normals. Neither is wrong and they do not agree, so every
    // verb that reads a normal differs at the last bits on a curved surface.
    //
    // This asserts BOTH halves: that it still differs (an estimator silently
    // unified is a real change to what a brush does) and that it differs by far
    // less than the displacement itself (a divergence that grew would mean one
    // representation had drifted).
    const Mesh base = cube_sphere(10, 1.0f);

    // The pairing is by the pristine STARTING position, taken before either
    // surface is touched — matching after the stamp compares a moved vertex
    // against unmoved ones and reports the displacement itself as a
    // disagreement, which is how a working Draw once came to look like a 0.4
    // error.
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    const std::vector<cfloat3> start = positions_of(*surface);
    std::vector<std::size_t> pair(start.size(), 0);
    for (std::size_t i = 0; i < start.size(); ++i) {
        float best = 1e30f;
        for (std::size_t j = 0; j < base.positions.size(); ++j) {
            const float d = cdot2(base.positions[j] - start[i]);
            if (d < best) {
                best = d;
                pair[i] = j;
            }
        }
        REQUIRE(best < 1e-12f);
    }

    struct Row {
        MeshBrush verb;
        const char* name;
    };
    const Row rows[] = {{MeshBrush::Draw, "draw"},
                        {MeshBrush::Inflate, "inflate"},
                        {MeshBrush::Clay, "clay"}};

    for (const Row& row : rows) {
        CAPTURE(row.name);

        Mesh fixed_mesh = base;
        MeshSculptor fixed(fixed_mesh, 1e-5f);
        auto adaptive_surface = DynamicSurface::from_mesh(base);
        REQUIRE(adaptive_surface.has_value());
        DynamicSculptor adaptive(*adaptive_surface);

        MeshBrushSettings b;
        b.center = cf3(0, 0, 1.0f);
        b.radius = 0.5f;
        b.strength = 0.5f;
        b.geodesic = false;

        const std::size_t f = fixed.stamp(row.verb, b);
        const std::size_t d = adaptive.stamp(row.verb, b, topology_off()).moved_vertices;

        // THE SAME WORK, on the same vertices: the estimators differ, the reach
        // does not.
        CHECK(f == d);
        CHECK(f == 21);

        const std::vector<cfloat3> got = positions_of(*adaptive_surface);
        std::size_t differing = 0;
        float worst = 0.0f;
        float largest_move = 0.0f;
        for (std::size_t i = 0; i < got.size(); ++i) {
            const cfloat3 mine = fixed_mesh.positions[pair[i]];
            if (!same_bits(got[i], mine)) ++differing;
            worst = std::max(worst, clength(got[i] - mine));
            largest_move = std::max(largest_move, clength(got[i] - start[i]));
        }
        CAPTURE(differing);
        CAPTURE(worst);
        CAPTURE(largest_move);

        // IT DIFFERS. Measured at 9, 7 and 9 of the 21 moved vertices.
        CHECK(differing > 0);
        // ...AND ONLY AT THE LAST BITS. Measured worst-case 1.2e-9, 6.0e-8 and
        // 1.4e-9 against displacements of order 0.1, so the bound is four
        // orders of magnitude clear of the noise and would fail loudly on a
        // representation that had actually drifted.
        CHECK(worst < 1e-5f);
        CHECK(largest_move > 1e-3f);
        CHECK(worst < largest_move * 1e-3f);
    }
}

// -- the arena is the sculptor's, and it converges ----------------------------

TEST_CASE("adaptive parity: the adaptive arena converges over a stroke") {
    // THE ARENA CLAIM WHERE IT IS ACTUALLY EXERCISED. The fixed sculptor's
    // arena reads all zeroes for a stamp whose automask needs no flood — its
    // scratch is members — so a `growths` assertion there is trivially true.
    // Every adaptive stamp sorts its region through the arena, so this is the
    // representation the convergence claim is worth making on.
    const Mesh base = plane_grid(16, 1.0f);
    auto surface = DynamicSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    DynamicSculptor adaptive(*surface);

    MeshBrushSettings b = euclidean_brush();
    b.strength = 0.05f;

    for (int i = 0; i < 6; ++i) {
        b.center = cf3(-0.0625f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f);
        adaptive.stamp(MeshBrush::Grab, b, topology_off());
    }
    const std::size_t warm = adaptive.arena().growths();
    const std::size_t capacity = adaptive.arena().capacity_bytes();
    REQUIRE(warm > 0);
    REQUIRE(capacity > 0);

    for (int i = 0; i < 40; ++i) {
        b.center = cf3(-0.0625f + 0.03125f * static_cast<float>(i % 6), 0.0f, 0.0f);
        adaptive.stamp(MeshBrush::Grab, b, topology_off());
    }

    CAPTURE(warm);
    CAPTURE(adaptive.arena().growths());
    CHECK(adaptive.arena().growths() == warm);
    CHECK(adaptive.arena().capacity_bytes() == capacity);
    // The high water is a peak of one stamp and not a running total of forty.
    CHECK(adaptive.arena().high_water_bytes() <= capacity);
}

TEST_CASE("adaptive parity: two sculptors stamp concurrently without aliasing") {
    // THE PER-SCULPTOR ARENA CLAIM IS A THREADING CLAIM, and the case below
    // makes it sequentially, which is the half a `std::thread` is not needed
    // for. This is the other half: a shared mutable arena would be a data race
    // the first time a host stamped two layers on two threads, and nothing in
    // the design forbids a host from doing that.
    //
    // RUN THIS UNDER TSAN. Sequentially it asserts only that the results are
    // what a single-threaded run produces — which a racing arena would often
    // still deliver, because a bump allocator handing out overlapping blocks
    // corrupts scratch rather than reliably changing an answer. The gate is the
    // sanitizer; the assertions are what makes the sanitizer have something to
    // watch.
    const Mesh base = plane_grid(16, 1.0f);
    const MeshBrushSettings b = euclidean_brush();

    // The single-threaded answer, first, so the concurrent run has something to
    // be identical to rather than merely self-consistent.
    std::vector<cfloat3> expected;
    {
        auto surface = DynamicSurface::from_mesh(base);
        REQUIRE(surface.has_value());
        DynamicSculptor sculptor(*surface);
        for (int i = 0; i < 8; ++i) sculptor.stamp(MeshBrush::Grab, b, topology_off());
        expected = positions_of(*surface);
    }

    auto one = DynamicSurface::from_mesh(base);
    auto two = DynamicSurface::from_mesh(base);
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    DynamicSculptor first(*one);
    DynamicSculptor second(*two);

    auto stamp_eight = [&b](DynamicSculptor* sculptor) {
        for (int i = 0; i < 8; ++i) sculptor->stamp(MeshBrush::Grab, b, topology_off());
    };

    std::thread a(stamp_eight, &first);
    std::thread c(stamp_eight, &second);
    a.join();
    c.join();

    const std::vector<cfloat3> got_one = positions_of(*one);
    const std::vector<cfloat3> got_two = positions_of(*two);
    REQUIRE(got_one.size() == expected.size());
    REQUIRE(got_two.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(same_bits(got_one[i], expected[i]));
        CHECK(same_bits(got_two[i], expected[i]));
    }

    // Each arena spent its own storage, and neither is the other's.
    CHECK(&first.arena() != &second.arena());
    CHECK(first.arena().growths() > 0);
    CHECK(second.arena().growths() > 0);
}

TEST_CASE("adaptive parity: two sculptors do not share an arena") {
    // ONE PER SCULPTOR, NEVER A PROCESS-GLOBAL. A shared mutable arena would
    // make two stamps on two surfaces alias each other's scratch and would be a
    // data race the first time a host stamped two layers on two threads.
    // Nothing in the design forbids that, so this asserts the arenas are
    // genuinely separate objects with separate statistics.
    const Mesh base = plane_grid(16, 1.0f);
    auto one = DynamicSurface::from_mesh(base);
    auto two = DynamicSurface::from_mesh(base);
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    DynamicSculptor a(*one);
    DynamicSculptor b(*two);

    CHECK(&a.arena() != &b.arena());
    CHECK(a.arena().growths() == 0);
    CHECK(b.arena().growths() == 0);

    MeshBrushSettings s = euclidean_brush();
    a.stamp(MeshBrush::Grab, s, topology_off());

    CHECK(a.arena().growths() > 0);
    CHECK(b.arena().growths() == 0);
    CHECK(b.arena().capacity_bytes() == 0);
}
