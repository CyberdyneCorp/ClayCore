// Smooth voxel display (#108).
//
// The gallery is what found the problem — every voxel render was cubes while
// every SDF render was clay — so the assertions here are the ones a picture
// would show: a box rounds, a lone voxel survives, colour blends, and the
// blocky mesher is untouched.
#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <vector>

#include "clay/voxel/grid.h"
#include "kernel_utils.h"

using namespace clay;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

struct Extent {
    float lo = 1e30f, hi = -1e30f;
};

Extent x_extent(const mesh::Mesh& m) {
    Extent e;
    for (const kernel::cfloat3& p : m.positions) {
        e.lo = std::min(e.lo, p.x);
        e.hi = std::max(e.hi, p.x);
    }
    return e;
}

}  // namespace

TEST_CASE("a solid box rounds rather than staying a box") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(kernel::cf3(1, 1, 1));
    g.fill_box({0, 0, 0}, {7, 7, 7}, c);

    const mesh::Mesh blocky = g.mesh_greedy();
    const mesh::Mesh smooth = g.mesh_smooth();

    REQUIRE(smooth.positions.size() > 0);
    REQUIRE(smooth.indices.size() > 0);

    // Rounding shows at the CORNERS, not on the faces. A face-centre crossing
    // between an occupied and an empty cell interpolates to the midpoint, which
    // is exactly the plane greedy emits — so the two meshes reach the same
    // extent on each axis, and asserting otherwise would be asserting a shrink.
    const Extent b = x_extent(blocky), s = x_extent(smooth);
    CHECK(s.lo == doctest::Approx(b.lo));
    CHECK(s.hi == doctest::Approx(b.hi));

    // What changes is the corner. Distance from the box centre to the furthest
    // vertex is sqrt(3)/2 * side for a box and strictly less once the corners
    // are pulled in along their diagonals.
    const kernel::cfloat3 mid = kernel::cf3(0.4f, 0.4f, 0.4f);
    auto furthest = [&](const mesh::Mesh& m) {
        float d = 0;
        for (const kernel::cfloat3& p : m.positions) d = std::max(d, kernel::clength(p - mid));
        return d;
    };
    const float blocky_corner = furthest(blocky), smooth_corner = furthest(smooth);
    CHECK(blocky_corner == doctest::Approx(std::sqrt(3.0f) * 0.4f).epsilon(0.01));
    CHECK(smooth_corner < blocky_corner * 0.95f);

    // A rounding, not a collapse: the form still fills most of the box.
    CHECK(smooth_corner > blocky_corner * 0.6f);
}

TEST_CASE("a lone voxel survives the smoothing") {
    // The property that decides the default. Surface nets over the centroid of
    // a cell's crossings cannot erase an isolated voxel — it has a sign change
    // on each of its six edges — whereas a blur puts it near 0.3 occupancy,
    // under the isolevel, and it disappears.
    VoxelGrid g(0.1f);
    g.set({0, 0, 0}, g.palette_add(kernel::cf3(0, 1, 0)));

    const mesh::Mesh smooth = g.mesh_smooth();
    CHECK(smooth.positions.size() > 0);
    CHECK(smooth.indices.size() > 0);
}

TEST_CASE("a blur is what erases a thin feature, which is why it is not the default") {
    // Stated as a test because the proposal claims it: the difference between
    // the default and one blur pass is whether a lone voxel exists at all.
    VoxelGrid g(0.1f);
    g.set({0, 0, 0}, g.palette_add(kernel::cf3(0, 1, 0)));

    CHECK(g.mesh_smooth().positions.size() > 0);
    CHECK(g.mesh_smooth(VoxelGrid::SmoothOptions{1}).positions.empty());
}

TEST_CASE("colour blends between two palette regions") {
    VoxelGrid g(0.1f);
    const std::uint8_t red = g.palette_add(kernel::cf3(1, 0, 0));
    const std::uint8_t blue = g.palette_add(kernel::cf3(0, 0, 1));
    g.fill_box({0, 0, 0}, {3, 3, 3}, red);
    g.fill_box({4, 0, 0}, {7, 3, 3}, blue);

    const mesh::Mesh m = g.mesh_smooth();
    REQUIRE(m.colors.size() == m.positions.size());

    bool saw_blend = false;
    for (const kernel::cfloat3& c : m.colors) {
        // Nothing outside the palette: every channel is a convex combination
        // of (1,0,0) and (0,0,1), so green stays 0 and red+blue stays <= 1.
        CHECK(c.y == doctest::Approx(0.0f));
        CHECK(c.x <= doctest::Approx(1.0f));
        CHECK(c.z <= doctest::Approx(1.0f));
        if (c.x > 0.2f && c.z > 0.2f) saw_blend = true;
    }
    // A vertex on the seam sits between occupied cells of both colours.
    CHECK(saw_blend);
}

TEST_CASE("smoothing is the caller's choice and does not touch the grid") {
    VoxelGrid g(0.1f);
    g.fill_box({0, 0, 0}, {5, 5, 5}, g.palette_add(kernel::cf3(1, 1, 1)));

    const std::vector<std::uint8_t> before = g.serialize();
    const std::size_t occupied = g.occupied_count();

    (void)g.mesh_smooth();
    (void)g.mesh_smooth(VoxelGrid::SmoothOptions{2});
    (void)g.mesh_greedy();

    CHECK(g.occupied_count() == occupied);
    CHECK(g.serialize() == before);
}

TEST_CASE("an empty grid meshes to nothing, both ways") {
    VoxelGrid g(0.1f);
    CHECK(g.mesh_smooth().positions.empty());
    CHECK(g.mesh_smooth().indices.empty());
    CHECK(g.mesh_greedy().positions.empty());
}

TEST_CASE("the smooth mesh sits where the blocky one does") {
    // Lattice points are voxel CENTRES, so the mesher's origin carries a half
    // cell. Without it the smooth sculpt would sit half a voxel off the blocky
    // one on every axis, which a host toggling the display would see as the
    // model jumping.
    VoxelGrid g(0.1f);
    g.fill_box({0, 0, 0}, {9, 9, 9}, g.palette_add(kernel::cf3(1, 1, 1)));

    const Extent b = x_extent(g.mesh_greedy());
    const Extent s = x_extent(g.mesh_smooth());
    const float b_mid = 0.5f * (b.lo + b.hi), s_mid = 0.5f * (s.lo + s.hi);
    CHECK(s_mid == doctest::Approx(b_mid).epsilon(0.02));
}

TEST_CASE("a level meshes smoothly at its own cell size") {
    VoxelGrid g(0.2f);
    g.fill_box({0, 0, 0}, {5, 5, 5}, g.palette_add(kernel::cf3(1, 1, 1)));
    REQUIRE(g.add_level() == 1);

    const mesh::Mesh coarse = g.mesh_smooth(0);
    const mesh::Mesh fine = g.mesh_smooth(1);
    REQUIRE(coarse.positions.size() > 0);
    REQUIRE(fine.positions.size() > 0);

    // The same solid: the finer level describes it with more vertices at half
    // the cell size, over the same world extent.
    CHECK(fine.positions.size() > coarse.positions.size());
    const Extent c = x_extent(coarse), f = x_extent(fine);
    CHECK(f.lo == doctest::Approx(c.lo).epsilon(0.2));
    CHECK(f.hi == doctest::Approx(c.hi).epsilon(0.2));

    CHECK(g.mesh_smooth(9).positions.empty());  // a level the grid does not have
}
