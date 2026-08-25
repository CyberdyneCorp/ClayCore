#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "clay/io/handoff.h"
#include "clay/mesh/mesh_data.h"
#include "clay/voxel/mask.h"

// THE SCULPT HANDOFF (file-io spec: add-sculpt-handoff-export).
//
// The format is CyberRemesherAndUV's, not ours. These tests check the two
// things a writer can get wrong on its own — and both were found by reading
// their READER rather than by reasoning from this side, which is why neither
// was in the task list before.

using namespace clay;
using kernel::cf3;

namespace {

std::string header_of(const std::vector<std::uint8_t>& bytes) {
    const std::string all(bytes.begin(), bytes.end());
    const std::size_t end = all.find("end_header");
    return end == std::string::npos ? all : all.substr(0, end + 10);
}

// A quad mesh, with `indices` as the triangulation of the same quads — the
// invariant mesh_data.h states and their reader depends on.
mesh::Mesh two_quads() {
    mesh::Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(1, 1, 0), cf3(0, 1, 0),
                   cf3(2, 0, 0), cf3(3, 0, 0), cf3(3, 1, 0), cf3(2, 1, 0)};
    m.quads = {0, 1, 2, 3, 4, 5, 6, 7};
    m.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    return m;
}

}  // namespace

TEST_CASE("handoff: the header declares the version and the producer") {
    // Without the version comment their reader answers UnsupportedFormat by
    // design — it is what tells a handoff from an ordinary PLY.
    mesh::Mesh m = two_quads();
    io::HandoffOptions o;
    o.binary = false;
    const std::string h = header_of(io::save_handoff_ply(m, o));

    CHECK(h.find("comment cyber_sculpt_handoff 1 0") != std::string::npos);
    CHECK(h.find("comment cyber_handoff_producer claycore") != std::string::npos);
    // Every property their spec requires.
    CHECK(h.find("property float nx") != std::string::npos);
    CHECK(h.find("property uchar red") != std::string::npos);
    CHECK(h.find("property float material_mix") != std::string::npos);
}

TEST_CASE("handoff: a quad mesh is written as TRIANGLES") {
    // THE HAZARD READING THEIR READER FOUND. save_ply declares a mesh's QUADS
    // as its faces, and their reader rejects any face that is not a triangle —
    // "a sculpt export that is not triangulated is a producer bug". So our best
    // export is exactly the file they would refuse.
    mesh::Mesh m = two_quads();
    REQUIRE(m.quad_count() == 2);  // non-degenerate: there ARE quads to mishandle

    io::HandoffOptions o;
    o.binary = false;
    const std::vector<std::uint8_t> bytes = io::save_handoff_ply(m, o);
    const std::string all(bytes.begin(), bytes.end());

    // Four faces of three corners, not two of four.
    CHECK(all.find("element face 4") != std::string::npos);
    CHECK(all.find("\n4 ") == std::string::npos);  // no quad row anywhere
    CHECK(all.find("3 0 1 2") != std::string::npos);
}

TEST_CASE("handoff: a mesh with no normals gains them, and is not modified") {
    mesh::Mesh m = two_quads();
    REQUIRE(m.normals.empty());  // non-degenerate: there is nothing to copy

    io::HandoffOptions o;
    o.binary = false;
    const std::string all_bytes = [&] {
        const std::vector<std::uint8_t> b = io::save_handoff_ply(m, o);
        return std::string(b.begin(), b.end());
    }();
    CHECK(all_bytes.find("property float nx") != std::string::npos);
    // The quads lie in the z = 0 plane, so every normal is +/-Z and none is the
    // zero vector a naive implementation would emit.
    CHECK((all_bytes.find(" 0.000000 0.000000 1.000000 ") != std::string::npos ||
           all_bytes.find(" 0.000000 0.000000 -1.000000 ") != std::string::npos));
    // WRITING DOES NOT MODIFY: the caller's mesh still has none.
    CHECK(m.normals.empty());
}

TEST_CASE("handoff: the material mix comes from a mask, and is zero without one") {
    mesh::Mesh m = two_quads();
    // A mask covering the FIRST quad only, so the test has both halves. A mask
    // covering everything, or nothing, would compare a column against itself.
    voxel::MaskField mask(0.25f);
    mask.fill(math::Aabb{cf3(-0.5f, -0.5f, -0.5f), cf3(1.5f, 1.5f, 0.5f)}, 1.0f);
    REQUIRE(mask.painted_count() > 0);

    const std::vector<float> mixed = io::handoff_material_mix(m, &mask);
    REQUIRE(mixed.size() == m.positions.size());
    // Inside the painted box.
    CHECK(mixed[0] > 0.5f);
    // Outside it: the second quad sits at x = 2..3.
    CHECK(mixed[4] == 0.0f);

    // And with no mask the payload is still THERE, just zero — it is required.
    const std::vector<float> none = io::handoff_material_mix(m, nullptr);
    CHECK(none.size() == m.positions.size());
    for (float v : none) CHECK(v == 0.0f);
}

TEST_CASE("handoff: binary and ascii describe the same mesh") {
    mesh::Mesh m = two_quads();
    io::HandoffOptions a, b;
    a.binary = false;
    b.binary = true;
    const std::string ha = header_of(io::save_handoff_ply(m, a));
    const std::string hb = header_of(io::save_handoff_ply(m, b));
    CHECK(ha.find("format ascii") != std::string::npos);
    CHECK(hb.find("format binary_little_endian") != std::string::npos);
    // Same counts and the same property set, whichever encoding.
    CHECK(ha.find("element vertex 8") != std::string::npos);
    CHECK(hb.find("element vertex 8") != std::string::npos);
    CHECK(ha.find("element face 4") != std::string::npos);
    CHECK(hb.find("element face 4") != std::string::npos);
}

TEST_CASE("handoff: vertex normals are area-weighted and unit length") {
    // The helper the writer needs, tested on its own. A sliver triangle must not
    // count as much as the large face beside it, which is the shape a
    // marching-cubes lattice produces along a diagonal.
    mesh::Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0), cf3(1, 1, 0)};
    m.indices = {0, 1, 2, 1, 3, 2};
    const std::vector<kernel::cfloat3> n = mesh::vertex_normals(m);
    REQUIRE(n.size() == 4);
    for (const kernel::cfloat3& v : n) {
        CHECK(kernel::clength(v) == doctest::Approx(1.0f));
        CHECK(std::fabs(v.z) == doctest::Approx(1.0f));  // the plane's normal
    }
}

TEST_CASE("handoff: a vertex no triangle touches gets a direction, not a zero") {
    // A zero normal is not a direction, and a consumer that normalises it gets
    // NaN — which reaches their bake as a black texel rather than an error.
    mesh::Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0), cf3(5, 5, 5)};
    m.indices = {0, 1, 2};
    const std::vector<kernel::cfloat3> n = mesh::vertex_normals(m);
    CHECK(kernel::clength(n[3]) == doctest::Approx(1.0f));
}
