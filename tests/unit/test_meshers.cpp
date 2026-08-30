#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/validate.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using mesh::Mesh;
using mesh::ValidationReport;

namespace {

scene::Tape sphere_tape(float r) {
    static scene::Document doc;
    doc = scene::Document{};
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::sphere(r), cf3(0, 0, 0));
    n.color = cf3(0.8f, 0.2f, 0.1f);
    l.sdf->insert(n);
    return scene::compile_document(doc);
}

// two hard-unioned boxes forming a plus sign: sharp 90-degree edges. Rotated
// off the lattice axes — an axis-aligned box is degenerate for this test
// (marching's lerp crossings on axis lattice edges are exact there).
const math::Quat kCrossRot = math::Quat::from_axis_angle(cf3(1, 0, 0), 0.35f);

scene::Tape cross_tape() {
    static scene::Document doc;
    doc = scene::Document{};
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node a = item(scene::Prim::box(cf3(0.6f, 0.3f, 0.3f)), cf3(0, 0, 0));
    scene::Node b = item(scene::Prim::box(cf3(0.3f, 0.6f, 0.3f)), cf3(0, 0, 0));
    a.xform.rotation = kCrossRot;
    b.xform.rotation = kCrossRot;
    l.sdf->insert(a);
    l.sdf->insert(b);
    return scene::compile_document(doc);
}

}  // namespace

TEST_CASE("mesh_tape: a resolution it cannot afford is refused, not allocated") {
    // The dense lattice is sized from the caller's voxel size. Without a
    // ceiling, a fine one either overflows the float-to-int conversion or ends
    // the process in the allocator — the library builds without exceptions, so
    // std::bad_alloc reaches std::terminate rather than returning.
    scene::Tape t = sphere_tape(1.0f);
    const math::Aabb region{cf3(-1.5f, -1.5f, -1.5f), cf3(1.5f, 1.5f, 1.5f)};

    for (float bad : {0.0f, -0.1f, 1e-7f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        Mesh m = mesh::mesh_tape(t, region, bad);
        CHECK(m.empty());
    }

    // a resolution it can afford still meshes
    Mesh ok = mesh::mesh_tape(t, region, 0.1f);
    CHECK_FALSE(ok.empty());

    // The ceiling has to clear what the API DOCUMENTS, not merely something
    // comfortable: docs/05-claycore-library.md's headline call meshes at
    // resolution 512, which is 514^3 lattice points over a cubic region. Set
    // too low, the guard turns the documented usage into an error.
    static_assert(mesh::kMaxGridSamples >= 514ull * 514ull * 514ull,
                  "the grid ceiling must admit the documented resolution of 512");
}

TEST_CASE("surface nets: sphere preview is lighter than marching, outward-oriented") {
    scene::Tape tape = sphere_tape(1.0f);
    math::Aabb region{cf3(-1.3f, -1.3f, -1.3f), cf3(1.3f, 1.3f, 1.3f)};
    Mesh nets = mesh::mesh_tape_nets(tape, region, 0.05f);
    Mesh mt = mesh::mesh_tape(tape, region, 0.05f);
    REQUIRE(!nets.empty());
    REQUIRE(!mt.empty());

    // preview economy: one vertex per cell beats one per crossing edge, and
    // one quad per crossing edge beats the tetrahedra fan
    CHECK(nets.positions.size() < mt.positions.size());
    CHECK(nets.triangle_count() < mt.triangle_count());

    // NOT asserted: watertight/manifold — the meshing spec does not promise
    // them for surface nets. Where the sphere IS manifold, orientation must
    // be consistent and outward (positive signed volume near 4/3*pi).
    ValidationReport r = mesh::validate(nets);
    CHECK(r.oriented);
    CHECK(r.degenerate_triangles == 0);
    CHECK(mesh::signed_volume(nets) == doctest::Approx(4.18879).epsilon(0.05));

    // attribute pass shared with mesh_tape
    CHECK(nets.colors.size() == nets.positions.size());
    CHECK(nets.normals.size() == nets.positions.size());
    for (std::size_t i = 0; i < nets.positions.size(); i += 11) {
        cfloat3 radial = cnormalize(nets.positions[i]);
        CHECK(cdot(nets.normals[i], radial) > 0.99f);
    }
}

TEST_CASE("surface nets: region-crossing sphere stays closed (ring)") {
    scene::Tape tape = sphere_tape(1.0f);
    Mesh m = mesh::mesh_tape_nets(
        tape, math::Aabb{cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f)}, 0.05f);
    REQUIRE(!m.empty());
    ValidationReport r = mesh::validate(m);
    CHECK(r.boundary_edges == 0);  // clipped, but capped by the closure ring
    CHECK(mesh::signed_volume(m) > 0.0);
}

TEST_CASE("surface nets: lattice API leaves range-boundary surfaces open") {
    // sphere poking through the +x face of the cell range: no ring, so the
    // hole is expected — closure is the tape wrapper's job
    auto sample = [](int i, int j, int k) {
        cfloat3 p = cf3(-0.6f + 0.05f * (float)i, -0.6f + 0.05f * (float)j,
                        -0.6f + 0.05f * (float)k);
        return clength(p) - 0.5f;
    };
    int cmin[3] = {0, 0, 0};
    int cmax[3] = {12, 24, 24};  // cut at x = 0
    Mesh m = mesh::mesh_lattice_nets(sample, cmin, cmax, cf3(-0.6f, -0.6f, -0.6f), 0.05f);
    REQUIRE(!m.empty());
    CHECK(mesh::validate(m).boundary_edges > 0);
}

TEST_CASE("dual contouring: experimental flag off yields an empty mesh") {
    scene::Tape tape = cross_tape();
    math::Aabb region{cf3(-0.85f, -0.85f, -0.85f), cf3(0.85f, 0.85f, 0.85f)};
    Mesh m = mesh::mesh_tape_dc(tape, region, 0.05f);  // default options: flag off
    CHECK(m.empty());
}

TEST_CASE("dual contouring golden: hard-union box keeps sharp edges") {
    const float voxel = 0.05f;
    scene::Tape tape = cross_tape();
    math::Aabb region{cf3(-0.85f, -0.85f, -0.85f), cf3(0.85f, 0.85f, 0.85f)};
    mesh::DualContouringOptions opts;
    opts.enable_experimental = true;
    Mesh dc = mesh::mesh_tape_dc(tape, region, voxel, opts);
    Mesh mt = mesh::mesh_tape(tape, region, voxel);
    REQUIRE(!dc.empty());
    REQUIRE(!mt.empty());
    CHECK(mesh::validate(dc).boundary_edges == 0);  // scene inside region: closed
    CHECK(mesh::signed_volume(dc) > 0.0);           // outward winding

    // true box edge of the (0.6, 0.3, 0.3) box: local line (y, z) = (0.3, 0.3)
    // sampled over x in [0.35, 0.55] — safely outside the second box
    auto edge_dist = [](cfloat3 p) {
        cfloat3 lp = kCrossRot.conjugate().rotate(p);
        return csqrt((lp.y - 0.3f) * (lp.y - 0.3f) + (lp.z - 0.3f) * (lp.z - 0.3f));
    };
    auto near_edge = [&](cfloat3 p) {
        cfloat3 lp = kCrossRot.conjugate().rotate(p);
        return lp.x > 0.35f && lp.x < 0.55f && edge_dist(p) < 1.5f * voxel;
    };

    // QEF vertices land on the edge line: several within 15% of a voxel
    // (marching/nets round the edge by roughly half a voxel instead)
    int on_edge = 0;
    for (const cfloat3& p : dc.positions)
        if (near_edge(p) && edge_dist(p) < 0.15f * voxel) ++on_edge;
    CHECK(on_edge >= 2);

    // near the edge, DC vertices sit closer to the true surface than
    // marching's (|SDF| at a vertex = distance to the exact hard-union box)
    auto max_field_error = [&](const Mesh& m) {
        float worst = 0.0f;
        for (const cfloat3& p : m.positions)
            if (near_edge(p)) worst = cmax(worst, cabs(tape.eval(p).d));
        return worst;
    };
    float dc_err = max_field_error(dc);
    float mt_err = max_field_error(mt);
    CHECK(dc_err < mt_err);
    CHECK(dc_err < 0.15f * voxel);
}

TEST_CASE("dual contouring: lattice API with central-difference normals") {
    auto sample = [](int i, int j, int k) {
        cfloat3 p = cf3(-0.6f + 0.05f * (float)i, -0.6f + 0.05f * (float)j,
                        -0.6f + 0.05f * (float)k);
        return clength(p) - 0.5f;
    };
    int cmin[3] = {0, 0, 0};
    int cmax[3] = {24, 24, 24};
    Mesh m = mesh::mesh_lattice_dc(sample, cmin, cmax, cf3(-0.6f, -0.6f, -0.6f), 0.05f);
    REQUIRE(!m.empty());
    ValidationReport r = mesh::validate(m);
    CHECK(r.oriented);
    CHECK(r.degenerate_triangles == 0);
    CHECK(mesh::signed_volume(m) == doctest::Approx(0.5236).epsilon(0.05));  // 4/3*pi*0.5^3
}

TEST_CASE("the dual walk places a cell's vertex before any quad that references it") {
    // The property that lets the dual mesher place vertices and emit quads in
    // ONE walk of the cell range instead of two. A quad sits on a lattice edge
    // leaving a cell's min corner, and the four cells around that edge are
    // reached by stepping BACK along the two axes that are not the edge's --
    // never forward. So they are the owning cell and three already placed, and
    // the walk never needs a vertex it has not made yet.
    //
    // Read off the output as: the largest index in successive quads never goes
    // backwards. If a future change reordered the walk, or emitted quads for a
    // cell that owns no vertex, this is what would notice.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node a = item(scene::Prim::sphere(0.8f), cf3(0, 0, 0));
    scene::Node b = item(scene::Prim::box(cf3(0.5f, 0.4f, 0.6f)), cf3(0.6f, 0.3f, 0.0f));
    b.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
    l.sdf->insert(a);
    l.sdf->insert(b);
    const scene::Tape tape = scene::compile_document(doc);

    mesh::MeshingOptions options;
    options.normals = mesh::NormalMode::None;
    options.colors = false;
    const Mesh m = mesh::mesh_tape_quads(tape, tape.bounds, 0.03f, options);
    REQUIRE(m.quads.size() >= std::size_t{4000});  // a real surface, not a corner case

    std::uint32_t high_water = 0;
    for (std::size_t q = 0; q < m.quads.size(); q += 4) {
        CAPTURE(q);
        const std::uint32_t largest = std::max(std::max(m.quads[q], m.quads[q + 1]),
                                               std::max(m.quads[q + 2], m.quads[q + 3]));
        REQUIRE(largest < m.positions.size());
        REQUIRE(largest >= high_water);  // never references a vertex placed later
        high_water = largest;
    }
}

TEST_CASE("the public parallel lattice march equals the serial one byte for byte") {
    // WHAT THIS ADDS to "the parallel lattice march welds seams exactly like
    // the serial one" in test_mesh.cpp, which already holds the byte-identity
    // claim: that one reaches the parallel march through `mesh_tape`, because
    // until add-voxel-remesher there was no other way in. `mesh_lattice_parallel`
    // is now a declared entry point, so it is called DIRECTLY here — a caller
    // can hand it a sample function `mesh_tape` would never produce — and the
    // under-eight-planes case takes the serial fallback INSIDE the parallel
    // entry point, which nothing exercised before.
    //
    // Byte-identity by CONSTRUCTION rather than by tolerance: a slab records
    // its crossings without welding and one builder replays them in slab
    // order, so the builder sees exactly the call sequence the serial march
    // makes. That is a claim about a mechanism, so this compares bytes rather
    // than counts or volumes.
    //
    // The sample function is a pure read of its own arithmetic, which is that
    // entry point's stated precondition.
    auto sample = [](int i, int j, int k) {
        const cfloat3 p = cf3(-0.7f + 0.02f * (float)i, -0.7f + 0.02f * (float)j,
                              -0.7f + 0.02f * (float)k);
        // Not a sphere: a lumpy shape, so the march meets ambiguous-looking
        // sign patterns and long diagonal runs rather than one clean shell.
        return clength(p) - 0.5f + 0.08f * csin(9.0f * p.x) * csin(9.0f * p.y) *
                                       csin(9.0f * p.z);
    };
    // Well past kMinParallelPlanes (8) and past one wave (64), so the wave loop
    // and the slab seams are both exercised rather than the serial fallback.
    int cmin[3] = {0, 0, 0};
    int cmax[3] = {70, 70, 70};
    const cfloat3 origin = cf3(-0.7f, -0.7f, -0.7f);

    const Mesh serial = mesh::mesh_lattice(sample, cmin, cmax, origin, 0.02f);
    const Mesh parallel = mesh::mesh_lattice_parallel(sample, cmin, cmax, origin, 0.02f);

    // Non-empty is REQUIRED before the comparison and not merely likely: two
    // empty meshes compare equal and prove nothing, and `positions.data()` on
    // an empty vector is null — which `memcmp` may not be handed even for a
    // zero count, and UBSan says so.
    REQUIRE(serial.positions.size() > 0);
    REQUIRE(serial.positions.size() == parallel.positions.size());
    CHECK(std::memcmp(serial.positions.data(), parallel.positions.data(),
                      serial.positions.size() * sizeof(cfloat3)) == 0);
    CHECK(serial.indices == parallel.indices);

    // Fewer than eight planes takes the serial fallback INSIDE the parallel
    // entry point, which must agree too — that path is where a divergence would
    // hide, being the one nothing else exercises.
    //
    // The slab is placed on the sphere's equator (world z = -0.7 + 0.02k, so
    // k = 35 is z = 0) rather than at the range's start, where it would meet no
    // surface at all and compare two empty meshes to each other.
    int thin_min[3] = {0, 0, 33};
    int thin_max[3] = {70, 70, 37};
    const Mesh thin_serial = mesh::mesh_lattice(sample, thin_min, thin_max, origin, 0.02f);
    const Mesh thin_parallel =
        mesh::mesh_lattice_parallel(sample, thin_min, thin_max, origin, 0.02f);
    REQUIRE(thin_serial.positions.size() > 0);
    REQUIRE(thin_serial.positions.size() == thin_parallel.positions.size());
    CHECK(std::memcmp(thin_serial.positions.data(), thin_parallel.positions.data(),
                      thin_serial.positions.size() * sizeof(cfloat3)) == 0);
    CHECK(thin_serial.indices == thin_parallel.indices);
}
