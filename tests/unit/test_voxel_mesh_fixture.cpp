// Golden fixtures for VoxelGrid::mesh_greedy.
//
// mesh_greedy reads voxels through a per-chunk pointer rather than a hash
// lookup per probe, so the in-chunk index it computes by hand has to agree with
// cell_at everywhere cell_at is subtle: negative coordinates (fdiv/fmod_pos),
// cells sitting on a chunk seam on each axis, and levels other than the active
// one. The hashes below pin the exact buffers — a divergence in any of those
// places moves bytes rather than counts, and counts alone would not catch it.
//
// Regenerating: these are outputs, not designed values. If a deliberate change
// to the mesher moves them, print the observed hash and re-baseline in the same
// commit as the behaviour change — never to make a red test green.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/voxel/grid.h"
#include "kernel_utils.h"

using namespace clay;
using namespace clay::kernel;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void u64(std::uint64_t v) { bytes(&v, sizeof(v)); }
    // An empty vector's data() may be null, and hashing zero bytes from null is
    // undefined even though it reads nothing — UBSan gates CI on exactly that.
    template <typename T>
    void vec(const std::vector<T>& v) {
        u64(v.size());
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
};

struct Digest {
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::uint64_t hash = 0;
};

Digest digest(const mesh::Mesh& m) {
    Fnv f;
    f.vec(m.positions);
    f.vec(m.normals);
    f.vec(m.colors);
    f.vec(m.uvs);
    f.vec(m.indices);
    return {m.positions.size(), m.indices.size(), f.h};
}

// Eight palette entries, so merged quads are broken up by color the way a real
// sculpt breaks them up and the mask carries more than one nonzero value.
//
// Every component is a multiple of 1/16, which is exact in binary32, because a
// golden hash covers the color bytes and the fixture has to produce the same
// bytes on every target. The obvious spelling, 1.0f - 0.1f * i, does not: the
// compiler is free to contract it to one fused multiply-add, arm64 has the
// instruction and x86-64's baseline does not, and the two round differently —
// 1.0f - 0.1f * 7 is 0x3e99999a contracted and 0x3e999999 not. That is a
// one-ulp disagreement in a value nothing here is measuring, and it turned
// every fixture carrying a high palette index red on macOS while the mesher
// under test was byte-identical. Exact operands take the question away rather
// than answering it per platform.
std::vector<std::uint8_t> seed_palette(VoxelGrid& g) {
    std::vector<std::uint8_t> pal;
    for (int i = 0; i < 8; ++i) {
        const float t = static_cast<float>(i) / 16.0f;
        pal.push_back(g.palette_add(cf3(t + 1.0f / 16.0f, 0.375f, 1.0f - t)));
    }
    return pal;
}

// A blobby form: three overlapping balls, unioned, tested in integer lattice
// space. The shape only has to be a large multi-chunk body whose merged quads
// meet at seams; deciding it with an integer predicate rather than a float SDF
// keeps the occupancy — and so the golden hash — identical everywhere. A float
// distance would put cells whose centre falls within an ulp of the surface on
// whichever side the target's libm and fused arithmetic land, which is a
// coin-flip in the fixture rather than a property of the mesher.
bool in_blob(int x, int y, int z, int scale) {
    auto ball = [&](int cx, int cy, int cz, int r) {
        const long long dx = x - cx, dy = y - cy, dz = z - cz;
        return dx * dx + dy * dy + dz * dz <= static_cast<long long>(r) * r;
    };
    return ball(0, 0, 0, scale) || ball(9 * scale / 10, 2 * scale / 5, 0, 3 * scale / 5) ||
           ball(-3 * scale / 10, 4 * scale / 5, scale / 2, scale / 2);
}

void fill_blob(VoxelGrid& g, float vs, const std::vector<std::uint8_t>& pal, int shift) {
    // The lattice radius one world unit buys at this cell size: the fixture is
    // the same shape at every cell size, sampled more finely.
    const int scale = static_cast<int>(1.0f / vs);
    const int r = 2 * scale;
    for (int z = -r; z <= r; ++z)
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                if (!in_blob(x, y, z, scale)) continue;
                int oct = (x >= 0 ? 1 : 0) | (y >= 0 ? 2 : 0) | (z >= 0 ? 4 : 0);
                g.set({x + shift, y + shift, z + shift}, pal[static_cast<std::size_t>(oct)]);
            }
}

void check_digest(const mesh::Mesh& m, std::size_t vertices, std::size_t indices,
                  std::uint64_t hash) {
    Digest d = digest(m);
    CHECK(d.vertices == vertices);
    CHECK(d.indices == indices);
    CHECK(d.hash == hash);
}

}  // namespace

TEST_CASE("mesh_greedy fixture: single cell at the origin") {
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({0, 0, 0}, pal[0]);
    check_digest(g.mesh_greedy(), 24, 36, 0xf2cd840d57c66007ull);
}

TEST_CASE("mesh_greedy fixture: single cell in negative space") {
    // fdiv/fmod_pos territory: chunk key -1, in-chunk offset 31 on every axis.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({-1, -1, -1}, pal[1]);
    check_digest(g.mesh_greedy(), 24, 36, 0xa7c9443e44c0fb47ull);
}

TEST_CASE("mesh_greedy fixture: two cells far apart") {
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({0, 0, 0}, pal[0]);
    g.set({2048, 2048, 2048}, pal[2]);
    check_digest(g.mesh_greedy(), 48, 72, 0x3ee4f3a1cf5452d3ull);
}

TEST_CASE("mesh_greedy fixture: two cells far apart across the origin") {
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({-2048, -33, 5}, pal[3]);
    g.set({2048, 2048, -2048}, pal[4]);
    check_digest(g.mesh_greedy(), 48, 72, 0x7a2bada08b602943ull);
}

TEST_CASE("mesh_greedy fixture: cells on every chunk seam") {
    // Both sides of the seam at -32, 0, 32 and 64 on each axis in turn, so the
    // face probe crosses a chunk boundary in both directions on all three axes.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    const int b[] = {-33, -32, -31, -1, 0, 1, 30, 31, 32, 33, 63, 64, 65};
    for (int x : b) g.set({x, 0, 0}, pal[0]);
    for (int y : b) g.set({0, y, 0}, pal[1]);
    for (int z : b) g.set({0, 0, z}, pal[2]);
    check_digest(g.mesh_greedy(), 336, 504, 0x0f52545de81dc4abull);
}

TEST_CASE("mesh_greedy fixture: solid blocks straddling a seam") {
    // Solid material spanning a seam, so the covered-face test fires ACROSS the
    // boundary rather than only inside a chunk. One block on the positive side,
    // one on the negative side.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    for (int z = 28; z <= 36; ++z)
        for (int y = 28; y <= 36; ++y)
            for (int x = 28; x <= 36; ++x)
                g.set({x, y, z}, pal[static_cast<std::size_t>((x + y + z) & 7)]);
    for (int z = -4; z <= 4; ++z)
        for (int y = -4; y <= 4; ++y)
            for (int x = -4; x <= 4; ++x)
                g.set({x - 32, y, z}, pal[static_cast<std::size_t>((x * 3 + y) & 7)]);
    check_digest(g.mesh_greedy(), 2736, 4104, 0x229055f5f32db2c1ull);
}

TEST_CASE("mesh_greedy fixture: rasterized blob in the positive octant") {
    VoxelGrid g(0.05f);
    auto pal = seed_palette(g);
    fill_blob(g, 0.05f, pal, 40);
    check_digest(g.mesh_greedy(), 17652, 26478, 0xdc2f919a9c08d1e6ull);
}

TEST_CASE("mesh_greedy fixture: rasterized blob straddling the origin") {
    VoxelGrid g(0.05f);
    auto pal = seed_palette(g);
    fill_blob(g, 0.05f, pal, 0);
    check_digest(g.mesh_greedy(), 17652, 26478, 0xc8861b257edd50b6ull);
}

TEST_CASE("mesh_greedy fixture: every level of a multi-level grid") {
    // Each level owns its own chunk map, so a sweep that read the wrong one
    // would still return a plausible mesh. Pin all three.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    REQUIRE(g.add_level() == 1);
    REQUIRE(g.add_level() == 2);
    REQUIRE(g.set_active_level(2));
    for (int z = -6; z <= 6; ++z)
        for (int y = -6; y <= 6; ++y)
            for (int x = -6; x <= 6; ++x)
                if (x * x + y * y + z * z <= 36)
                    g.set({x + 30, y - 30, z}, pal[static_cast<std::size_t>((x + 4) & 7)]);

    check_digest(g.mesh_greedy(0), 164, 246, 0x7e89417cdf6dbd03ull);
    check_digest(g.mesh_greedy(1), 500, 750, 0x24a084d43602bd2eull);
    check_digest(g.mesh_greedy(2), 1496, 2244, 0x554c9cddf7f88d10ull);
    // The level overload and the active-level form must agree.
    CHECK(digest(g.mesh_greedy()).hash == digest(g.mesh_greedy(2)).hash);
    // A level this grid does not have meshes to nothing rather than reading out
    // of range.
    CHECK(g.mesh_greedy(3).empty());
}

TEST_CASE("mesh_greedy fixture: one cell in each of 64 chunks") {
    // The sparse case the slab grouping exists for, and the one where a mask
    // window spanning mostly empty space has to stay both cheap and correct.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    for (int i = 0; i < 64; ++i)
        g.set({(i - 32) * 32, ((i * 7) % 9 - 4) * 32, ((i * 5) % 7 - 3) * 32},
              pal[static_cast<std::size_t>(i & 7)]);
    check_digest(g.mesh_greedy(), 1536, 2304, 0x32dddc5a8b52c392ull);
}

TEST_CASE("mesh_greedy reads empty space without creating it") {
    // The sweep walks a slab window that is mostly coordinates in no chunk at
    // all. write_cell erases a chunk that reaches zero occupancy, so a missing
    // chunk IS empty — a sweep that materialized one to read it would grow the
    // grid every time the host displayed it.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    for (int i = 0; i < 16; ++i)
        g.set({(i - 8) * 32, (i % 5 - 2) * 32, (i % 3 - 1) * 32},
              pal[static_cast<std::size_t>(i & 7)]);
    const std::size_t occupied = g.occupied_count();
    const std::vector<std::uint8_t> before = g.serialize();

    Digest first = digest(g.mesh_greedy());
    CHECK(g.occupied_count() == occupied);
    CHECK(g.serialize() == before);
    // And meshing again returns the same mesh rather than one grown by the
    // first pass.
    Digest second = digest(g.mesh_greedy());
    CHECK(second.hash == first.hash);
    CHECK(second.vertices == first.vertices);
    CHECK(second.indices == first.indices);
}

TEST_CASE("mesh_greedy fixture: an empty grid meshes to nothing") {
    VoxelGrid g(0.1f);
    mesh::Mesh m = g.mesh_greedy();
    CHECK(m.positions.empty());
    CHECK(m.indices.empty());
    CHECK(m.empty());
}
