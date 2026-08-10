// Resolution levels on a voxel grid (voxel-engine spec).
//
// The load-bearing claims, in order of how much damage getting them wrong
// would do: a single-level grid is exactly the grid that existed before; a
// level change is not destructive; a world-addressed mask selects the same
// region at every level; the dither stays reproducible per level.

#include <doctest/doctest.h>

#include <algorithm>
#include <set>

#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

using namespace clay;
using namespace clay::kernel;
using voxel::BrushFalloff;
using voxel::BrushParams;
using voxel::BrushShape;
using voxel::MaskField;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

// Every cell of a level, so two grids can be compared without caring how the
// chunks happen to be laid out.
std::set<std::tuple<int, int, int, int>> level_cells(const VoxelGrid& g, std::size_t level) {
    VoxelGrid copy = g;
    copy.set_active_level(level);
    std::set<std::tuple<int, int, int, int>> out;
    auto lo = copy.bounds_min();
    auto hi = copy.bounds_max();
    if (!lo || !hi) return out;
    for (int z = lo->z; z <= hi->z; ++z)
        for (int y = lo->y; y <= hi->y; ++y)
            for (int x = lo->x; x <= hi->x; ++x) {
                std::uint8_t v = copy.get({x, y, z});
                if (v != 0) out.insert({x, y, z, v});
            }
    return out;
}

// Whether the world point is inside material, asked of one level.
bool solid_at(const VoxelGrid& g, std::size_t level, cfloat3 p) {
    VoxelGrid copy = g;
    copy.set_active_level(level);
    return copy.sample_step_field(p) < 0.0f;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + i]) << (i * 8);
    return v;
}

// Offset just past the coarsest level's chunk records — where a build that
// predates the level tail stops reading.
std::size_t end_of_base_level(const std::vector<std::uint8_t>& b) {
    std::size_t pos = 4;  // cell size
    std::uint32_t palette = read_u32(b, pos);
    pos += 4 + 12 * palette;
    std::uint32_t chunks = read_u32(b, pos);
    pos += 4;
    for (std::uint32_t i = 0; i < chunks; ++i) pos += 16 + read_u32(b, pos + 12);
    return pos;
}

}  // namespace

TEST_CASE("a grid without levels is the grid that existed before") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 0, 0));
    g.set_brush({0, 0, 0}, 5, c, BrushShape::Sphere);

    CHECK(g.level_count() == 1);
    CHECK(g.active_level() == 0);
    CHECK(g.voxel_size() == doctest::Approx(0.1f));
    CHECK(g.drop_level() == false);          // a grid always has at least one
    CHECK(g.set_active_level(1) == false);   // and no second one to select
    CHECK(g.level_count() == 1);

    SUBCASE("the serialised bytes carry no tail") {
        // The stream is the layout it always was — cell size, palette, one RLE
        // record per chunk — and stops there. Anything appended shows up here.
        const std::vector<std::uint8_t> bytes = g.serialize();
        CHECK(end_of_base_level(bytes) == bytes.size());
    }

    SUBCASE("adding then dropping a level restores the bytes exactly") {
        const std::vector<std::uint8_t> before = g.serialize();
        g.add_level();
        CHECK(g.serialize() != before);  // the tail is really there
        CHECK(g.drop_level());
        CHECK(g.serialize() == before);
    }
}

TEST_CASE("a single-level grid serialises to the bytes it did before levels existed") {
    // The golden below was taken from a build that predates levels, running
    // this exact construction. It is here because "a single-level grid is
    // unchanged" is the one property this feature is not allowed to break, and
    // every other check in this file would still pass if the stream had
    // quietly grown a field.
    VoxelGrid g(0.1f);
    std::uint8_t r = g.palette_add(cf3(1, 0, 0));
    std::uint8_t b = g.palette_add(cf3(0, 0.25f, 1));
    g.set_brush({0, 0, 0}, 7, r, BrushShape::Sphere);
    g.fill_box({-40, -3, 12}, {-35, 2, 20}, b);
    g.fill_line({-30, -30, -30}, {30, 30, 30}, r);
    BrushParams p;
    p.size = 9;
    p.shape = BrushShape::Sphere;
    p.falloff = BrushFalloff::Linear;  // no transcendental: exactly reproducible
    p.strength = 0.55f;
    p.seed = 4242u;
    g.set_brush({5, 5, 5}, p, b);

    const std::vector<std::uint8_t> bytes = g.serialize();
    CHECK(g.occupied_count() == 617);
    CHECK(bytes.size() == 1603);
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a
    for (std::uint8_t v : bytes) {
        hash ^= v;
        hash *= 1099511628211ull;
    }
    CHECK(hash == 5314600681519063811ull);
}

TEST_CASE("adding a level does not move the surface") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(0.2f, 0.4f, 0.8f));
    g.set_brush({0, 0, 0}, 6, c, BrushShape::Sphere);
    const std::size_t coarse_cells = g.occupied_count();

    std::size_t fine = g.add_level();
    CHECK(fine == 1);
    CHECK(g.level_count() == 2);
    CHECK(g.active_level() == 0);  // adding a level does not change which is active
    CHECK(g.level_voxel_size(1) == doctest::Approx(0.05f));
    // Eight children per coarse cell: the same solid at twice the resolution.
    CHECK(g.level_occupied_count(1) == coarse_cells * 8);

    // Sampled in WORLD space, the two levels agree everywhere.
    for (float z = -0.35f; z < 0.35f; z += 0.011f)
        for (float y = -0.35f; y < 0.35f; y += 0.011f)
            for (float x = -0.35f; x < 0.35f; x += 0.011f) {
                cfloat3 p = cf3(x, y, z);
                REQUIRE(solid_at(g, 0, p) == solid_at(g, 1, p));
            }
}

TEST_CASE("a stroke survives a round trip through another level") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 0));
    g.set_brush({0, 0, 0}, 6, c, BrushShape::Sphere);
    g.add_level();

    const auto coarse_before = level_cells(g, 0);
    const auto fine_before = level_cells(g, 1);

    // Down to fine and back with nothing done in between.
    CHECK(g.set_active_level(1));
    CHECK(g.set_active_level(0));
    CHECK(level_cells(g, 0) == coarse_before);
    CHECK(level_cells(g, 1) == fine_before);

    SUBCASE("a stroke made at the fine level shows up coarse and comes back whole") {
        CHECK(g.set_active_level(1));
        g.set_brush({8, 0, 0}, 4, c, BrushShape::Cube);
        const auto fine_after = level_cells(g, 1);
        CHECK(fine_after != fine_before);

        // Averaged down: the 4^3 block of fine cells at (8..11) fills the 2^3
        // coarse cells at (4..5), each of whose eight children are occupied.
        CHECK(g.set_active_level(0));
        CHECK(g.get({4, 0, 0}) == c);
        CHECK(g.set_active_level(1));
        CHECK(level_cells(g, 1) == fine_after);  // the trip through coarse changed nothing
    }
}

TEST_CASE("fine detail survives a coarse edit") {
    VoxelGrid g(0.1f);
    std::uint8_t body = g.palette_add(cf3(0.5f, 0.5f, 0.5f));
    std::uint8_t stud = g.palette_add(cf3(1, 0, 0));
    g.fill_box({0, 0, 0}, {7, 7, 7}, body);
    g.add_level();

    // Detail at the finest level: a stud proud of the slab, and a pit into it.
    CHECK(g.set_active_level(1));
    const VoxelCoord proud{4, 16, 4};   // one cell above the slab's top face
    const VoxelCoord pit{6, 15, 6};     // one cell into it
    g.set(proud, stud);
    g.set(pit, 0);
    REQUIRE(g.get(proud) == stud);
    REQUIRE(g.get(pit) == 0);

    // A broad stroke at the coarse level, well away from the detail.
    CHECK(g.set_active_level(0));
    const std::uint64_t before = g.change_count();
    g.fill_box({0, 8, 0}, {7, 9, 7}, body);  // two more coarse layers on top
    CHECK(g.change_count() > before);

    CHECK(g.set_active_level(1));
    SUBCASE("the broad stroke is present at the fine level") {
        CHECK(g.get({0, 18, 0}) == body);  // inside the new coarse layers
        CHECK(g.get({15, 19, 15}) == body);
    }
    SUBCASE("and the fine detail is still there") {
        // The pit is inside the slab and untouched by the stroke.
        CHECK(g.get(pit) == 0);
        // The stud is now buried by the broad stroke, which is what it means
        // for the coarse form to move under detail that rides on it.
        CHECK(g.get(proud) == stud);
    }
}

TEST_CASE("a coarse edit under detail leaves the detail standing") {
    VoxelGrid g(0.2f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.fill_box({0, 0, 0}, {3, 3, 3}, c);
    g.add_level();

    CHECK(g.set_active_level(1));
    const VoxelCoord notch{2, 2, 2};
    g.set(notch, 0);  // a one-cell notch, invisible at the coarse level
    // Averaging keeps the coarse cell: one child in eight is not a majority.
    CHECK(g.level_occupied_count(0) == 64);

    CHECK(g.set_active_level(0));
    g.set({1, 1, 1}, c);  // rewriting a cell it already holds
    CHECK(g.set_active_level(1));
    CHECK(g.get(notch) == 0);  // the notch is not filled in by the replay
}

TEST_CASE("a world-addressed mask selects the same region at every level") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(0, 1, 0));
    g.fill_box({-4, -4, -4}, {3, 3, 3}, c);
    g.add_level();

    // The mask's own lattice matches neither level, which is the point: it is
    // addressed in world units and both levels sample it through world space.
    MaskField mask(0.037f);
    mask.fill(math::Aabb{cf3(-1.0f, -1.0f, -1.0f), cf3(0.0f, 1.0f, 1.0f)}, 1.0f);

    BrushParams p;
    p.size = 16;
    p.mask = &mask;

    VoxelGrid coarse = g, fine = g;
    coarse.set_active_level(0);
    coarse.erase_brush({0, 0, 0}, p);
    fine.set_active_level(1);
    fine.erase_brush({0, 0, 0}, p);

    // Everything at x >= 0 in world space is erased at both levels; everything
    // the mask covers survives at both.
    for (int i = -4; i <= 3; ++i) {
        CAPTURE(i);
        const bool masked = i < 0;
        CHECK((coarse.get({i, 0, 0}) != 0) == masked);
        CHECK((fine.get({2 * i, 0, 0}) != 0) == masked);
        CHECK((fine.get({2 * i + 1, 0, 0}) != 0) == masked);
    }
}

TEST_CASE("the dither is reproducible per level") {
    BrushParams p;
    p.size = 9;
    p.shape = BrushShape::Sphere;
    p.falloff = BrushFalloff::Smooth;
    p.strength = 0.6f;
    p.seed = 20240607u;

    auto stamp = [&](std::size_t level) {
        VoxelGrid g(0.1f);
        std::uint8_t c = g.palette_add(cf3(1, 0, 1));
        g.add_level();
        g.set_active_level(level);
        g.set_brush({3, 3, 3}, p, c);
        return level_cells(g, level);
    };

    const auto once = stamp(1);
    const auto twice = stamp(1);
    CHECK(once == twice);       // same stamp, same seed, same cells
    CHECK(!once.empty());
    // A different level is a different lattice, so it is a different — but
    // equally reproducible — set of cells.
    CHECK(stamp(0) == stamp(0));
}

TEST_CASE("levels round-trip through the grid stream") {
    VoxelGrid g(0.1f);
    std::uint8_t body = g.palette_add(cf3(0.3f, 0.3f, 0.3f));
    std::uint8_t trim = g.palette_add(cf3(0.9f, 0.1f, 0.1f));
    g.fill_box({0, 0, 0}, {5, 5, 5}, body);
    g.add_level();
    g.add_level();
    CHECK(g.set_active_level(2));
    g.set({7, 24, 7}, trim);
    g.set({8, 23, 8}, 0);
    CHECK(g.set_active_level(1));
    g.set({3, 12, 3}, trim);
    CHECK(g.set_active_level(2));

    const std::vector<std::uint8_t> bytes = g.serialize();
    auto back = VoxelGrid::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(back->level_count() == 3);
    CHECK(back->active_level() == 2);
    CHECK(back->voxel_size() == doctest::Approx(0.025f));
    for (std::size_t level = 0; level < 3; ++level) {
        CAPTURE(level);
        CHECK(level_cells(*back, level) == level_cells(g, level));
    }
    CHECK(back->serialize() == bytes);  // and it is a fixed point

    SUBCASE("a reader that stops at the coarsest level gets a single-level grid") {
        // Exactly what a build predating the tail does: it consumes the chunk
        // records and never looks further.
        VoxelGrid coarse_only(0.1f);
        coarse_only.palette_add(cf3(0.3f, 0.3f, 0.3f));
        coarse_only.palette_add(cf3(0.9f, 0.1f, 0.1f));
        coarse_only.fill_box({0, 0, 0}, {5, 5, 5}, body);
        const std::vector<std::uint8_t> head = coarse_only.serialize();
        REQUIRE(head.size() < bytes.size());
        CHECK(std::equal(head.begin(), head.end(), bytes.begin()));

        auto opened = VoxelGrid::deserialize(head.data(), head.size());
        REQUIRE(opened.has_value());
        CHECK(opened->level_count() == 1);
        CHECK(opened->voxel_size() == doctest::Approx(0.1f));
        CHECK(level_cells(*opened, 0) == level_cells(g, 0));
    }
}

TEST_CASE("the level stack is capped") {
    // Empty, so the cap is reached without paying 8^15 cells for it. The cap
    // exists because the file format needs one: a stream may name any count.
    VoxelGrid g(1.0f);
    for (std::size_t i = 1; i < VoxelGrid::kMaxLevels; ++i) CHECK(g.add_level() == i);
    CHECK(g.level_count() == VoxelGrid::kMaxLevels);

    CHECK(g.add_level() == VoxelGrid::kMaxLevels - 1);  // the finest it already had
    CHECK(g.level_count() == VoxelGrid::kMaxLevels);    // and the stack did not grow
}

TEST_CASE("a malformed level tail is refused rather than half-read") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 0, 0));
    g.fill_box({0, 0, 0}, {2, 2, 2}, c);
    g.add_level();
    g.set_active_level(1);
    g.set({0, 0, 0}, 0);
    const std::vector<std::uint8_t> bytes = g.serialize();

    SUBCASE("truncated inside the tail") {
        for (std::size_t cut = bytes.size() - 1; cut + 12 > bytes.size(); --cut) {
            std::vector<std::uint8_t> cropped(bytes.begin(), bytes.begin() + cut);
            CHECK(!VoxelGrid::deserialize(cropped.data(), cropped.size()).has_value());
        }
    }
    SUBCASE("an offset naming a palette index the file does not carry") {
        std::vector<std::uint8_t> broken = bytes;
        broken.back() = 200;
        CHECK(!VoxelGrid::deserialize(broken.data(), broken.size()).has_value());
    }
}

TEST_CASE("meshing chooses a level") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.fill_box({0, 0, 0}, {1, 1, 1}, c);
    g.add_level();

    mesh::Mesh coarse = g.mesh_greedy(0);
    mesh::Mesh fine = g.mesh_greedy(1);
    // The same box at both levels: one merged quad per face either way, and the
    // world-space extent is identical because the cells halved with the count.
    CHECK(coarse.indices.size() == fine.indices.size());
    float coarse_max = 0.0f, fine_max = 0.0f;
    for (const cfloat3& p : coarse.positions) coarse_max = std::max(coarse_max, p.x);
    for (const cfloat3& p : fine.positions) fine_max = std::max(fine_max, p.x);
    CHECK(coarse_max == doctest::Approx(fine_max));

    CHECK(g.mesh_greedy(7).indices.empty());  // a level the grid does not have
    CHECK(g.mesh_greedy().indices.size() == coarse.indices.size());  // the active one
}

TEST_CASE("a tail cannot ask for more cells than it supplies") {
    // Regression: the level tail is a fixed handful of bytes however deep the
    // stack it names, and every level above 0 is rebuilt by subdividing. So a
    // stream that simply claims sixteen levels over a small coarsest level is a
    // request for 8^15 cells from a file that is a few hundred bytes long, and
    // it used to be honoured — deserialize did not return.
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    g.fill_box({0, 0, 0}, {3, 3, 3}, c);
    REQUIRE(g.occupied_count() == 64);
    const std::vector<std::uint8_t> honest = g.serialize();

    auto forged = [&](std::uint32_t levels) {
        std::vector<std::uint8_t> bytes = honest;
        auto put32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
        };
        put32(0x564C4343u);  // "CCLV"
        put32(levels);
        put32(0);                                                 // active
        for (std::uint32_t i = 1; i < levels; ++i) put32(0);      // no detail at any level
        return bytes;
    };

    SUBCASE("a depth the coarsest level cannot pay for is refused") {
        const std::vector<std::uint8_t> bomb = forged(VoxelGrid::kMaxLevels);
        CHECK(bomb.size() < 256);  // the whole point: tiny in, unbounded out
        CHECK(!VoxelGrid::deserialize(bomb.data(), bomb.size()).has_value());
    }

    SUBCASE("a depth it can pay for still opens") {
        // 64 cells over three further levels is 64 * (8 + 64 + 512) — well
        // inside the budget, so the guard rejects the bomb and not the format.
        const std::vector<std::uint8_t> fine = forged(4);
        auto back = VoxelGrid::deserialize(fine.data(), fine.size());
        REQUIRE(back.has_value());
        CHECK(back->level_count() == 4);
        CHECK(back->level_occupied_count(3) == 64 * 512);
    }

    SUBCASE("an empty coarsest level costs nothing at any depth") {
        // The cap test builds a deep stack on an empty grid; that stays legal
        // because subdividing nothing is nothing.
        VoxelGrid empty(1.0f);
        for (std::size_t i = 1; i < VoxelGrid::kMaxLevels; ++i) empty.add_level();
        const std::vector<std::uint8_t> bytes = empty.serialize();
        auto back = VoxelGrid::deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->level_count() == VoxelGrid::kMaxLevels);
    }
}
