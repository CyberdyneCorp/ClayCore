#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// The full validation report across the ABI (c-abi spec: the full mesh
// validation report crosses the ABI; meshing spec: every validation primitive
// is reachable by a CONSUMER).
//
// What this is a regression for: mesh::ValidationReport carries eleven fields
// and clay_mesh_validate returned two of them, so a host could be told an
// export was bad and never told why. The meshing spec has always said the
// validator "reports the open edge loop" for a hole, and the number that says
// so — boundary_edges — did not cross. Nor did the self-intersection pass,
// whose cap the engine has always accepted and which neither binding ever
// passed, so it had never run outside this repository's own tests.

namespace {

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

// A closed tetrahedron, wound so every face normal points OUTWARD — which is
// what makes the signed volume positive and the orientation check meaningful.
const float kTetraPositions[12] = {
    0.0f, 0.0f, 0.0f,  // v0
    1.0f, 0.0f, 0.0f,  // v1
    0.0f, 1.0f, 0.0f,  // v2
    0.0f, 0.0f, 1.0f,  // v3
};
const std::uint32_t kTetraIndices[12] = {
    0, 2, 1,  // base, normal -z
    0, 1, 3,  // normal -y
    0, 3, 2,  // normal -x
    1, 2, 3,  // the slanted face, normal +1,+1,+1
};

void tetra(MeshHandle* out, bool with_hole) {
    // Dropping the last face leaves a hole bounded by its three edges.
    const std::size_t indices = with_hole ? 9 : 12;
    REQUIRE(clay_mesh_from_triangles(kTetraPositions, 4, kTetraIndices, indices, &out->m) ==
            CLAY_OK);
}

clay_validation_report report_of(const clay_mesh* m, std::size_t budget) {
    clay_validation_report r;
    r.struct_size = sizeof(r);
    REQUIRE(clay_mesh_validation_report(m, budget, &r) == CLAY_OK);
    return r;
}

}  // namespace

TEST_CASE("c abi: a closed mesh reports every quantity, not two booleans") {
    MeshHandle mesh;
    tetra(&mesh, false);
    const clay_validation_report r = report_of(mesh.m, 0);

    CHECK(r.vertices == 4);
    CHECK(r.triangles == 4);
    CHECK(r.watertight == 1);
    CHECK(r.manifold == 1);
    CHECK(r.oriented == 1);
    CHECK(r.clean == 1);
    CHECK(r.boundary_edges == 0);
    CHECK(r.non_manifold_edges == 0);
    CHECK(r.degenerate_triangles == 0);
    CHECK(r.euler_characteristic == 2);  // V - E + F for a closed surface of genus 0
}

TEST_CASE("c abi: a hole is REPORTED, not merely detected") {
    // The scenario the meshing spec has always stated and the ABI could not
    // answer: the caller learns how many edges are open, not only that
    // something is wrong.
    MeshHandle mesh;
    tetra(&mesh, true);
    const clay_validation_report r = report_of(mesh.m, 0);

    CHECK(r.watertight == 0);
    CHECK(r.clean == 0);
    CHECK(r.triangles == 3);
    CHECK(r.boundary_edges == 3);  // the three edges of the face that was dropped
    // Still edge-manifold: an open mesh is not a broken one, and conflating
    // the two is what a single "is it bad" bit forces a host to do.
    CHECK(r.manifold == 1);
    CHECK(r.non_manifold_edges == 0);
}

TEST_CASE("c abi: the self-intersection pass is reachable, and says whether it ran") {
    // Two triangles crossing through each other's interior, sharing no vertex
    // so the adjacency skip does not swallow the pair.
    const float positions[18] = {
        -1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,   // in z = 0
        0.0f,  0.2f, -1.0f, 0.0f, 0.2f, 1.0f,  0.0f, 0.9f, 0.0f,   // in x = 0
    };
    const std::uint32_t indices[6] = {0, 1, 2, 3, 4, 5};
    MeshHandle mesh;
    REQUIRE(clay_mesh_from_triangles(positions, 6, indices, 6, &mesh.m) == CLAY_OK);

    // Without a budget the pass does not run. The count is zero because
    // nothing looked, and the echoed budget is the only thing that says so.
    const clay_validation_report skipped = report_of(mesh.m, 0);
    CHECK(skipped.intersecting_pairs == 0);
    CHECK(skipped.intersection_budget == 0);

    // With one, it runs — and this is the first time any consumer of the
    // library has been able to ask for it.
    const clay_validation_report tested = report_of(mesh.m, 1000);
    CHECK(tested.intersecting_pairs == 1);
    CHECK(tested.intersection_budget == 1000);
    CHECK(tested.clean == 0);
}

TEST_CASE("c abi: the two-boolean entry point is unchanged") {
    MeshHandle closed;
    tetra(&closed, false);
    MeshHandle holed;
    tetra(&holed, true);

    for (const MeshHandle* m : {&closed, &holed}) {
        std::int32_t watertight = -1, manifold = -1;
        REQUIRE(clay_mesh_validate(m->m, &watertight, &manifold) == CLAY_OK);
        const clay_validation_report r = report_of(m->m, 0);
        CHECK(watertight == r.watertight);
        CHECK(manifold == r.manifold);
    }

    // Both outputs optional, as before.
    std::int32_t only = -1;
    CHECK(clay_mesh_validate(closed.m, &only, nullptr) == CLAY_OK);
    CHECK(only == 1);
    CHECK(clay_mesh_validate(closed.m, nullptr, nullptr) == CLAY_OK);
}

TEST_CASE("c abi: the report obeys the versioned-descriptor rule") {
    MeshHandle mesh;
    tetra(&mesh, false);

    // Below the layout: rejected rather than read at a shifted offset.
    clay_validation_report short_decl{};
    short_decl.struct_size = sizeof(short_decl) - 8;
    CHECK(clay_mesh_validation_report(mesh.m, 0, &short_decl) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_validation_report zero{};
    zero.struct_size = 0;
    CHECK(clay_mesh_validation_report(mesh.m, 0, &zero) == CLAY_ERROR_INVALID_ARGUMENT);

    // Absurdly large: the first word is not a struct_size at all.
    clay_validation_report huge{};
    huge.struct_size = 1u << 20;
    CHECK(clay_mesh_validation_report(mesh.m, 0, &huge) == CLAY_ERROR_INVALID_ARGUMENT);

    // A newer caller's tail is ignored, the write is clamped to what this
    // build knows, and the size the CALLER declared comes back — not ours.
    struct Padded {
        clay_validation_report report;
        std::uint32_t sentinel;
    };
    Padded padded{};
    padded.sentinel = 0xABCDEF01u;
    padded.report.struct_size = sizeof(clay_validation_report) + sizeof(std::uint32_t);
    CHECK(clay_mesh_validation_report(mesh.m, 0, &padded.report) == CLAY_OK);
    CHECK(padded.report.struct_size == sizeof(clay_validation_report) + sizeof(std::uint32_t));
    CHECK(padded.report.watertight == 1);
    CHECK(padded.sentinel == 0xABCDEF01u);

    CHECK(clay_mesh_validation_report(nullptr, 0, &padded.report) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_validation_report(mesh.m, 0, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_validation_report(mesh.m, static_cast<std::size_t>(CLAY_MAX_BATCH) + 1,
                                      &padded.report) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c abi: a mesh reports its volume and area, and the sign is the orientation") {
    MeshHandle mesh;
    tetra(&mesh, false);

    double volume = 0.0, area = 0.0;
    REQUIRE(clay_mesh_measure(mesh.m, &volume, &area) == CLAY_OK);
    CHECK(volume == doctest::Approx(1.0 / 6.0).epsilon(1e-5));
    // three unit right triangles at 0.5, plus the slanted face at sqrt(3)/2
    CHECK(area == doctest::Approx(1.5 + 0.8660254).epsilon(1e-5));

    // Reversing the winding negates it, which is what makes the sign usable as
    // an orientation check rather than a curiosity.
    std::vector<std::uint32_t> flipped(kTetraIndices, kTetraIndices + 12);
    for (std::size_t t = 0; t < 4; ++t) std::swap(flipped[t * 3 + 1], flipped[t * 3 + 2]);
    MeshHandle inverted;
    REQUIRE(clay_mesh_from_triangles(kTetraPositions, 4, flipped.data(), flipped.size(),
                                     &inverted.m) == CLAY_OK);
    double inverted_volume = 0.0;
    REQUIRE(clay_mesh_measure(inverted.m, &inverted_volume, nullptr) == CLAY_OK);
    CHECK(inverted_volume == doctest::Approx(-1.0 / 6.0).epsilon(1e-5));

    // Either output may be skipped; both null is the caller asking nothing.
    double only_area = 0.0;
    CHECK(clay_mesh_measure(mesh.m, nullptr, &only_area) == CLAY_OK);
    CHECK(only_area == doctest::Approx(area));
    CHECK(clay_mesh_measure(mesh.m, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_measure(nullptr, &volume, &area) == CLAY_ERROR_INVALID_ARGUMENT);
}
