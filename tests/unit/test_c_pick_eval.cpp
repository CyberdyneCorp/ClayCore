#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/kernel/field.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/surface_nets.h"
#include "clay/pick/pick.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "kernel_utils.h"

// The C ABI evaluation, picking and meshing surface (c-abi spec: picking and
// evaluation parity). Each case asks the same question twice — once through
// the C boundary, once of the engine directly the way the Python bindings ask
// it — and requires the same answer. Attribution in particular has to name the
// layer and node the engine names, or "what did I click on" means something
// different depending on which binding is driving.

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// Two layers holding one item each, built through the C boundary and mirrored
// on the scene model: a sphere at the origin and a box off to +X, far enough
// apart that a ray down either one attributes unambiguously.
struct TwoLayers {
    clay_document* doc = clay_document_create();
    clay_layer_id ball_layer = 0;
    clay_layer_id slab_layer = 0;
    clay_node_id ball = 0;
    clay_node_id slab = 0;
    scene::Document reference;

    TwoLayers() {
        REQUIRE(clay_add_sdf_layer(doc, "ball", &ball_layer) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(doc, "slab", &slab_layer) == CLAY_OK);
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<std::uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_SPHERE;
        d.params[0] = 1.0f;
        d.rotation[3] = 1.0f;
        d.scale = 1.0f;
        d.color[0] = 0.9f;
        REQUIRE(clay_add_item(doc, ball_layer, &d, &ball) == CLAY_OK);
        clay_item_desc b = d;
        b.prim = CLAY_PRIM_BOX;
        b.params[0] = b.params[1] = b.params[2] = 0.5f;
        b.position[0] = 3.0f;
        b.color[0] = 0.1f;
        b.color[2] = 0.8f;
        REQUIRE(clay_add_item(doc, slab_layer, &b, &slab) == CLAY_OK);

        scene::Layer& ref_ball = reference.add_sdf_layer("ball");
        scene::Node sphere;
        sphere.prim = scene::Prim::sphere(1.0f);
        sphere.color = cf3(0.9f, 0.0f, 0.0f);
        ref_ball.sdf->insert(sphere);
        scene::Layer& ref_slab = reference.add_sdf_layer("slab");
        scene::Node box;
        box.prim = scene::Prim::box(cf3(0.5f, 0.5f, 0.5f));
        box.xform.position = cf3(3.0f, 0.0f, 0.0f);
        box.color = cf3(0.1f, 0.0f, 0.8f);
        ref_slab.sdf->insert(box);
    }
    ~TwoLayers() { clay_document_destroy(doc); }
    TwoLayers(const TwoLayers&) = delete;
    TwoLayers& operator=(const TwoLayers&) = delete;
};

std::vector<float> sample_points() {
    std::vector<float> pts;
    clay_test::Lcg rng(9091);
    for (int i = 0; i < 120; ++i) {
        cfloat3 p = rng.vec3(-2.0f, 4.0f);
        pts.push_back(p.x);
        pts.push_back(p.y);
        pts.push_back(p.z);
    }
    return pts;
}

// A solid block of voxels, so a ray from any of the six directions has a
// surface to enter through.
struct CubeGrid {
    clay_voxel_grid* grid = clay_voxel_grid_create(0.1f);

    CubeGrid() {
        REQUIRE(grid != nullptr);
        const std::int32_t lo[3] = {0, 0, 0};
        const std::int32_t hi[3] = {3, 3, 3};
        std::int32_t index = 0;
        const float rgb[3] = {0.5f, 0.5f, 0.5f};
        REQUIRE(clay_voxel_palette_add(grid, rgb, &index) == CLAY_OK);
        REQUIRE(clay_voxel_fill_box(grid, lo, hi, index) == CLAY_OK);
    }
    ~CubeGrid() { clay_voxel_grid_destroy(grid); }
    CubeGrid(const CubeGrid&) = delete;
    CubeGrid& operator=(const CubeGrid&) = delete;
};

clay_mesh_params mesh_params(std::int32_t mesher, std::int32_t experimental) {
    clay_mesh_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.resolution = 32;
    p.mesher = mesher;
    p.experimental = experimental;
    return p;
}

}  // namespace

TEST_CASE("gradients through the C boundary are the normals the engine computes") {
    TwoLayers built;
    std::vector<float> pts = sample_points();
    const std::size_t n = pts.size() / 3;
    std::vector<float> got(pts.size(), 0.0f);
    REQUIRE(clay_eval_gradients(built.doc, nullptr, pts.data(), n, got.data()) == CLAY_OK);

    scene::Tape tape = scene::compile_document(built.reference);
    int diverged = 0;
    for (std::size_t i = 0; i < n; ++i) {
        cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        cfloat3 want = kernel::cnormal([&](cfloat3 s) { return tape.eval(s).d; }, p, 1e-4f);
        const float want_xyz[3] = {want.x, want.y, want.z};
        bool same = true;
        for (int c = 0; c < 3; ++c)
            same = same && got[i * 3 + c] == doctest::Approx(want_xyz[c]).epsilon(1e-6);
        if (same) continue;
        if (++diverged > 5) continue;
        FAIL_CHECK("gradient differs at (" << p.x << ", " << p.y << ", " << p.z << "): C ABI ("
                                          << got[i * 3] << ", " << got[i * 3 + 1] << ", "
                                          << got[i * 3 + 2] << "), engine (" << want.x << ", "
                                          << want.y << ", " << want.z << ")");
    }
    CHECK(diverged == 0);
    // a gradient is a direction, so every one of them is unit length
    for (std::size_t i = 0; i < n; ++i) {
        float len = std::sqrt(got[i * 3] * got[i * 3] + got[i * 3 + 1] * got[i * 3 + 1] +
                              got[i * 3 + 2] * got[i * 3 + 2]);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-5));
    }
}

TEST_CASE("the safe step scale is the compiled tape's") {
    TwoLayers built;
    float scale = 0.0f;
    REQUIRE(clay_safe_step_scale(built.doc, &scale) == CLAY_OK);
    CHECK(scale == doctest::Approx(scene::compile_document(built.reference).safe_step_scale()));
    CHECK(scale > 0.0f);
}

TEST_CASE("a batch raycast answers what the single one answers, direction length aside") {
    TwoLayers built;
    // the second ray's direction is deliberately not unit length: the batch
    // form normalizes, so t stays a world distance
    const float rays[12] = {0.0f, 0.0f, -5.0f, 0.0f, 0.0f, 1.0f,
                            3.0f, 5.0f, 0.0f,  0.0f, -7.5f, 0.0f};
    std::int32_t hits[2] = {0, 0};
    float t[2] = {0.0f, 0.0f};
    float positions[6] = {};
    float normals[6] = {};
    REQUIRE(clay_raycast_many(built.doc, rays, 2, hits, t, positions, normals) == CLAY_OK);
    CHECK(hits[0] == 1);
    CHECK(hits[1] == 1);

    for (std::size_t i = 0; i < 2; ++i) {
        float dir[3] = {rays[i * 6 + 3], rays[i * 6 + 4], rays[i * 6 + 5]};
        float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int c = 0; c < 3; ++c) dir[c] /= len;
        std::int32_t one_hit = 0;
        float one_t = 0.0f, one_pos[3] = {}, one_normal[3] = {};
        REQUIRE(clay_raycast(built.doc, rays + i * 6, dir, &one_hit, &one_t, one_pos,
                             one_normal) == CLAY_OK);
        CHECK(one_hit == hits[i]);
        CHECK(t[i] == doctest::Approx(one_t));
        for (int c = 0; c < 3; ++c) {
            CHECK(positions[i * 3 + c] == doctest::Approx(one_pos[c]));
            CHECK(normals[i * 3 + c] == doctest::Approx(one_normal[c]));
        }
    }

    // a ray with no direction is rejected rather than traced into NaN
    const float degenerate[6] = {0.0f, 0.0f, -5.0f, 0.0f, 0.0f, 0.0f};
    CHECK(clay_raycast_many(built.doc, degenerate, 1, hits, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_raycast_many(built.doc, nullptr, 0, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_OK);
}

TEST_CASE("snapped points are the points the engine snaps to") {
    TwoLayers built;
    std::vector<float> pts = sample_points();
    const std::size_t n = pts.size() / 3;
    std::vector<float> positions(pts.size(), 0.0f);
    std::vector<float> normals(pts.size(), 0.0f);
    std::vector<std::int32_t> ok(n, -1);
    REQUIRE(clay_snap_to_surface(built.doc, pts.data(), n, positions.data(), normals.data(),
                                 ok.data()) == CLAY_OK);

    scene::Tape tape = scene::compile_document(built.reference);
    int diverged = 0;
    int converged = 0;
    for (std::size_t i = 0; i < n; ++i) {
        pick::SnapResult want =
            pick::snap_to_surface(tape, cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]));
        const float want_pos[3] = {want.position.x, want.position.y, want.position.z};
        const float want_nor[3] = {want.normal.x, want.normal.y, want.normal.z};
        bool same = ok[i] == (want.ok ? 1 : 0);
        for (int c = 0; c < 3; ++c) {
            same = same && positions[i * 3 + c] == doctest::Approx(want_pos[c]);
            same = same && normals[i * 3 + c] == doctest::Approx(want_nor[c]);
        }
        converged += ok[i];
        if (!same && ++diverged <= 5)
            FAIL_CHECK("snap differs at sample " << i << ": C ABI (" << positions[i * 3] << ", "
                                                 << positions[i * 3 + 1] << ", "
                                                 << positions[i * 3 + 2] << ") ok=" << ok[i]
                                                 << ", engine (" << want.position.x << ", "
                                                 << want.position.y << ", " << want.position.z
                                                 << ") ok=" << want.ok);
    }
    CHECK(diverged == 0);
    INFO("converged samples: " << converged << " of " << n);
    CHECK(converged > 0);  // the flag is reported, not hard-wired to zero
}

TEST_CASE("layer bounds and selection bounds are the boxes the engine reports") {
    TwoLayers built;
    float lo[3] = {}, hi[3] = {};
    std::int32_t has_bounds = 0;
    REQUIRE(clay_layer_bounds(built.doc, built.slab_layer, lo, hi, &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 1);
    math::Aabb want = pick::layer_bounds(*built.reference.find_layer(built.slab_layer));
    CHECK(lo[0] == doctest::Approx(want.min.x));
    CHECK(lo[1] == doctest::Approx(want.min.y));
    CHECK(lo[2] == doctest::Approx(want.min.z));
    CHECK(hi[0] == doctest::Approx(want.max.x));
    CHECK(hi[1] == doctest::Approx(want.max.y));
    CHECK(hi[2] == doctest::Approx(want.max.z));

    // one node of that layer: the same box, because the layer holds one item
    const clay_node_id selection[1] = {built.slab};
    float sel_lo[3] = {}, sel_hi[3] = {};
    REQUIRE(clay_layer_selection_bounds(built.doc, built.slab_layer, selection, 1, sel_lo,
                                        sel_hi, &has_bounds) == CLAY_OK);
    CHECK(has_bounds == 1);
    math::Aabb want_sel = pick::selection_bounds(built.reference, built.slab_layer,
                                                 {built.slab});
    CHECK(sel_lo[0] == doctest::Approx(want_sel.min.x));
    CHECK(sel_hi[0] == doctest::Approx(want_sel.max.x));

    // an empty selection, and one naming a node the layer does not hold, are
    // answers rather than failures
    has_bounds = 1;
    CHECK(clay_layer_selection_bounds(built.doc, built.slab_layer, nullptr, 0, sel_lo, sel_hi,
                                      &has_bounds) == CLAY_OK);
    CHECK(has_bounds == 0);
    const clay_node_id stranger[1] = {424242};
    has_bounds = 1;
    CHECK(clay_layer_selection_bounds(built.doc, built.slab_layer, stranger, 1, sel_lo, sel_hi,
                                      &has_bounds) == CLAY_OK);
    CHECK(has_bounds == 0);

    // an empty layer has no bounds, a layer that is not there is not found
    clay_layer_id empty = 0;
    REQUIRE(clay_add_sdf_layer(built.doc, "empty", &empty) == CLAY_OK);
    has_bounds = 1;
    CHECK(clay_layer_bounds(built.doc, empty, lo, hi, &has_bounds) == CLAY_OK);
    CHECK(has_bounds == 0);
    CHECK(clay_layer_bounds(built.doc, 99999, lo, hi, &has_bounds) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_selection_bounds(built.doc, 99999, selection, 1, lo, hi, &has_bounds) ==
          CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("an attributed raycast names the layer and the item that were hit") {
    TwoLayers built;
    struct Shot {
        float origin[3];
        clay_layer_id layer;
        clay_node_id node;
    };
    const Shot shots[2] = {{{0.0f, 0.0f, -5.0f}, built.ball_layer, built.ball},
                           {{3.0f, 0.0f, -5.0f}, built.slab_layer, built.slab}};
    const float dir[3] = {0.0f, 0.0f, 1.0f};
    for (const Shot& shot : shots) {
        std::int32_t hit = 0;
        float t = 0.0f, position[3] = {}, normal[3] = {};
        clay_layer_id layer = 424242;
        clay_node_id node = 424242;
        REQUIRE(clay_raycast_attributed(built.doc, shot.origin, dir, &hit, &t, position, normal,
                                        &layer, &node) == CLAY_OK);
        REQUIRE(hit == 1);
        CHECK(layer == shot.layer);
        CHECK(node == shot.node);

        math::Ray ray{cf3(shot.origin[0], shot.origin[1], shot.origin[2]), cf3(0, 0, 1)};
        pick::SceneHit want = pick::raycast_scene(built.reference, ray);
        REQUIRE(want.hit);
        CHECK(t == doctest::Approx(want.t));
        CHECK(position[2] == doctest::Approx(want.position.z));
        CHECK(normal[2] == doctest::Approx(want.normal.z));
    }

    // a miss attributes nothing, and 0 is never a layer or a node id
    const float away[3] = {0.0f, 40.0f, -5.0f};
    std::int32_t hit = 1;
    clay_layer_id layer = 424242;
    clay_node_id node = 424242;
    REQUIRE(clay_raycast_attributed(built.doc, away, dir, &hit, nullptr, nullptr, nullptr,
                                    &layer, &node) == CLAY_OK);
    CHECK(hit == 0);
    CHECK(layer == 0);
    CHECK(node == 0);

    const float no_direction[3] = {0.0f, 0.0f, 0.0f};
    CHECK(clay_raycast_attributed(built.doc, away, no_direction, &hit, nullptr, nullptr, nullptr,
                                  nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("voxel picking reports the face the ray entered and the cell across it") {
    CubeGrid grid;
    // one shot per face of the block, each aimed at the middle of that face:
    // the entry face is the one clay.h names, and the adjacent cell is one
    // step back along the ray
    struct Shot {
        float origin[3];
        float dir[3];
        std::int32_t face;
        std::int32_t cell[3];
        std::int32_t adjacent[3];
    };
    const Shot shots[6] = {
        {{2.0f, 0.15f, 0.15f}, {-1, 0, 0}, CLAY_VOXEL_FACE_POS_X, {3, 1, 1}, {4, 1, 1}},
        {{-2.0f, 0.15f, 0.15f}, {1, 0, 0}, CLAY_VOXEL_FACE_NEG_X, {0, 1, 1}, {-1, 1, 1}},
        {{0.15f, 2.0f, 0.15f}, {0, -1, 0}, CLAY_VOXEL_FACE_POS_Y, {1, 3, 1}, {1, 4, 1}},
        {{0.15f, -2.0f, 0.15f}, {0, 1, 0}, CLAY_VOXEL_FACE_NEG_Y, {1, 0, 1}, {1, -1, 1}},
        {{0.15f, 0.15f, 2.0f}, {0, 0, -1}, CLAY_VOXEL_FACE_POS_Z, {1, 1, 3}, {1, 1, 4}},
        {{0.15f, 0.15f, -2.0f}, {0, 0, 1}, CLAY_VOXEL_FACE_NEG_Z, {1, 1, 0}, {1, 1, -1}},
    };
    for (const Shot& shot : shots) {
        std::int32_t hit = 0, face = -1, cell[3] = {}, adjacent[3] = {};
        float t = 0.0f;
        REQUIRE(clay_voxel_raycast(grid.grid, shot.origin, shot.dir, &hit, cell, &face, adjacent,
                                   &t) == CLAY_OK);
        REQUIRE(hit == 1);
        CHECK(face == shot.face);
        for (int c = 0; c < 3; ++c) {
            CHECK(cell[c] == shot.cell[c]);
            CHECK(adjacent[c] == shot.adjacent[c]);
        }
        CHECK(t > 0.0f);
    }

    // a ray that misses the block reports a miss, not an error
    const float away[3] = {5.0f, 5.0f, 5.0f};
    const float outward[3] = {1.0f, 1.0f, 1.0f};
    std::int32_t hit = 1;
    REQUIRE(clay_voxel_raycast(grid.grid, away, outward, &hit, nullptr, nullptr, nullptr,
                               nullptr) == CLAY_OK);
    CHECK(hit == 0);

    const float no_direction[3] = {0.0f, 0.0f, 0.0f};
    CHECK(clay_voxel_raycast(grid.grid, away, no_direction, &hit, nullptr, nullptr, nullptr,
                             nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_raycast(nullptr, away, outward, &hit, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("build-plane picking answers the cell under the ray on a Y-normal plane") {
    CubeGrid grid;
    const float origin[3] = {0.55f, 2.0f, 0.35f};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    std::int32_t hit = 0, cell[3] = {};
    REQUIRE(clay_voxel_build_plane_pick(grid.grid, origin, down, 0, &hit, cell) == CLAY_OK);
    REQUIRE(hit == 1);
    CHECK(cell[0] == 5);  // 0.55 / 0.1
    CHECK(cell[1] == 0);  // always the plane's own cell index
    CHECK(cell[2] == 3);

    voxel::VoxelGrid reference(0.1f);
    math::Ray ray{cf3(origin[0], origin[1], origin[2]), cf3(0, -1, 0)};
    std::optional<voxel::VoxelCoord> want = pick::pick_build_plane(reference, ray, 0);
    REQUIRE(want.has_value());
    CHECK(cell[0] == want->x);
    CHECK(cell[1] == want->y);
    CHECK(cell[2] == want->z);

    // parallel to the plane, and with the plane behind the origin: both are
    // misses rather than errors
    const float sideways[3] = {1.0f, 0.0f, 0.0f};
    hit = 1;
    REQUIRE(clay_voxel_build_plane_pick(grid.grid, origin, sideways, 0, &hit, cell) == CLAY_OK);
    CHECK(hit == 0);
    const float up[3] = {0.0f, 1.0f, 0.0f};
    hit = 1;
    REQUIRE(clay_voxel_build_plane_pick(grid.grid, origin, up, 0, &hit, cell) == CLAY_OK);
    CHECK(hit == 0);
}

TEST_CASE("each mesher is reachable and builds what the engine builds") {
    TwoLayers built;
    scene::Tape tape = scene::compile_document(built.reference);
    kernel::cfloat3 ext = tape.bounds.extent();
    float voxel = kernel::cmax(ext.x, kernel::cmax(ext.y, ext.z)) / 32.0f;

    clay_mesh_params marching = mesh_params(CLAY_MESHER_MARCHING, 0);
    clay_mesh* m = nullptr;
    REQUIRE(clay_document_mesh(built.doc, &marching, &m) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(m) == mesh::mesh_tape(tape, tape.bounds, voxel).positions.size());
    clay_mesh_destroy(m);

    clay_mesh_params nets = mesh_params(CLAY_MESHER_NETS, 0);
    clay_mesh* n = nullptr;
    REQUIRE(clay_document_mesh(built.doc, &nets, &n) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(n) ==
          mesh::mesh_tape_nets(tape, tape.bounds, voxel).positions.size());
    // surface nets is the preview mesher: fewer vertices at the same voxel size
    CHECK(clay_mesh_vertex_count(n) > 0);
    clay_mesh_destroy(n);

    clay_mesh_params dc = mesh_params(CLAY_MESHER_DUAL_CONTOURING, 1);
    clay_mesh* d = nullptr;
    REQUIRE(clay_document_mesh(built.doc, &dc, &d) == CLAY_OK);
    mesh::DualContouringOptions opts;
    opts.enable_experimental = true;
    CHECK(clay_mesh_vertex_count(d) ==
          mesh::mesh_tape_dc(tape, tape.bounds, voxel, opts).positions.size());
    clay_mesh_destroy(d);
}

TEST_CASE("the experimental mesher is refused rather than silently meshed") {
    TwoLayers built;
    clay_mesh_params gated = mesh_params(CLAY_MESHER_DUAL_CONTOURING, 0);
    clay_mesh* mesh = reinterpret_cast<clay_mesh*>(0x1);
    CHECK(clay_document_mesh(built.doc, &gated, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    // the engine answers an unflagged call with an empty mesh, which a line
    // later would read as "meshing produced no triangles": the message has to
    // be the gate's
    CHECK(std::string(clay_last_error()).find("experimental") != std::string::npos);
    CHECK(mesh == reinterpret_cast<clay_mesh*>(0x1));  // untouched on failure

    clay_mesh_params unknown = mesh_params(7, 1);
    CHECK(clay_document_mesh(built.doc, &unknown, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_mesh_params negative = mesh_params(-1, 1);
    CHECK(clay_document_mesh(built.doc, &negative, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
}

namespace {

// clay_mesh_params as it shipped in ABI 0.2.0, before the mesher fields were
// appended. A caller compiled against that layout declares this size, and the
// two fields it never heard of have to take their documented default — which
// is what makes CLAY_MESHER_MARCHING zero.
struct MeshParamsOriginal {
    std::uint32_t struct_size;
    float voxel_size;
    std::int32_t resolution;
    std::int32_t decimate;
    float decimate_ratio;
};
static_assert(sizeof(MeshParamsOriginal) < sizeof(clay_mesh_params),
              "the mesher fields were appended, so the original layout is shorter");

}  // namespace

TEST_CASE("a caller from before the mesher field still meshes, and meshes marching") {
    TwoLayers built;
    // deliberately on the heap and exactly the old size, so reading a field
    // past it is a heap overflow the sanitizer catches rather than whatever
    // happened to follow on the stack
    std::vector<unsigned char> storage(sizeof(MeshParamsOriginal), 0);
    auto* old = reinterpret_cast<MeshParamsOriginal*>(storage.data());
    old->struct_size = static_cast<std::uint32_t>(sizeof(MeshParamsOriginal));
    old->resolution = 32;
    clay_mesh* from_old = nullptr;
    REQUIRE(clay_document_mesh(built.doc, reinterpret_cast<const clay_mesh_params*>(old),
                               &from_old) == CLAY_OK);

    clay_mesh_params spelled_out = mesh_params(CLAY_MESHER_MARCHING, 0);
    clay_mesh* from_new = nullptr;
    REQUIRE(clay_document_mesh(built.doc, &spelled_out, &from_new) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(from_old) == clay_mesh_vertex_count(from_new));
    CHECK(clay_mesh_vertex_count(from_old) > 0);
    clay_mesh_destroy(from_old);
    clay_mesh_destroy(from_new);
}

TEST_CASE("a layer evaluates on its own, not through the stack it sits in") {
    // What the Python bindings' Layer.eval answers: only that layer compiles,
    // so the box in the other layer cannot move the sphere's field.
    TwoLayers built;
    std::vector<float> pts = sample_points();
    const std::size_t n = pts.size() / 3;
    std::vector<float> got(n, 0.0f), colors(pts.size(), 0.0f);
    REQUIRE(clay_layer_eval_points(built.doc, built.ball_layer, nullptr, pts.data(), n,
                                   got.data(), colors.data()) == CLAY_OK);

    REQUIRE(built.reference.layers.size() == 2);
    scene::Tape tape = scene::compile_layer(built.reference.layers[0]);  // "ball"
    int diverged = 0;
    for (std::size_t i = 0; i < n; ++i) {
        cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        kernel::CTapeValue want = tape.eval(p);
        if (got[i] == doctest::Approx(want.d).epsilon(1e-6) &&
            colors[i * 3] == doctest::Approx(want.color.x).epsilon(1e-6))
            continue;
        if (++diverged > 5) continue;
        FAIL_CHECK("layer sample differs at (" << p.x << ", " << p.y << ", " << p.z
                                               << "): C ABI " << got[i] << " rgb ("
                                               << colors[i * 3] << ", " << colors[i * 3 + 1]
                                               << ", " << colors[i * 3 + 2] << "), engine "
                                               << want.d << " rgb (" << want.color.x << ", "
                                               << want.color.y << ", " << want.color.z << ")");
    }
    CHECK(diverged == 0);

    // and it is genuinely not the document's field: the box shows in one and
    // not the other
    std::vector<float> whole(n, 0.0f);
    REQUIRE(clay_eval_points(built.doc, nullptr, pts.data(), n, whole.data(), nullptr) ==
            CLAY_OK);
    bool differs = false;
    for (std::size_t i = 0; i < n; ++i) differs = differs || std::abs(whole[i] - got[i]) > 1e-3f;
    CHECK(differs);

    std::vector<float> grads(pts.size(), 0.0f);
    REQUIRE(clay_layer_eval_gradients(built.doc, built.ball_layer, nullptr, pts.data(), n,
                                      grads.data()) == CLAY_OK);
    for (std::size_t i = 0; i < n; ++i) {
        cfloat3 p = cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
        cfloat3 want = kernel::cnormal([&](cfloat3 s) { return tape.eval(s).d; }, p, 1e-4f);
        CHECK(grads[i * 3] == doctest::Approx(want.x).epsilon(1e-6));
        CHECK(grads[i * 3 + 1] == doctest::Approx(want.y).epsilon(1e-6));
        CHECK(grads[i * 3 + 2] == doctest::Approx(want.z).epsilon(1e-6));
    }

    float scale = 0.0f;
    REQUIRE(clay_layer_safe_step_scale(built.doc, built.ball_layer, &scale) == CLAY_OK);
    CHECK(scale == doctest::Approx(tape.safe_step_scale()));

    // a layer that does not exist is not an empty field, it is an error
    std::vector<float> ignored(n, 0.0f);
    CHECK(clay_layer_eval_points(built.doc, 9999, nullptr, pts.data(), n, ignored.data(),
                                 nullptr) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_eval_gradients(built.doc, 9999, nullptr, pts.data(), n, ignored.data()) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_safe_step_scale(built.doc, 9999, &scale) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_eval_points(nullptr, built.ball_layer, nullptr, pts.data(), n,
                                 ignored.data(), nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}
