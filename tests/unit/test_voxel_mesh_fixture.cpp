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
std::vector<std::uint8_t> seed_palette(VoxelGrid& g) {
    std::vector<std::uint8_t> pal;
    for (int i = 0; i < 8; ++i)
        pal.push_back(g.palette_add(cf3(0.1f * static_cast<float>(i + 1), 0.35f,
                                        1.0f - 0.1f * static_cast<float>(i))));
    return pal;
}

// A blobby form: three spheres smooth-unioned, rasterized by cell centre.
float smin(float a, float b, float k) {
    float h = std::max(k - std::abs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
float blob(float x, float y, float z) {
    auto sph = [&](float cx, float cy, float cz, float r) {
        float dx = x - cx, dy = y - cy, dz = z - cz;
        return std::sqrt(dx * dx + dy * dy + dz * dz) - r;
    };
    float d = sph(0.0f, 0.0f, 0.0f, 1.0f);
    d = smin(d, sph(0.9f, 0.4f, 0.0f, 0.6f), 0.4f);
    d = smin(d, sph(-0.3f, 0.8f, 0.5f, 0.5f), 0.4f);
    return d;
}

void fill_blob(VoxelGrid& g, float vs, const std::vector<std::uint8_t>& pal, int shift) {
    int r = static_cast<int>(std::ceil(1.9f / vs)) + 1;
    for (int z = -r; z <= r; ++z)
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                float wx = (static_cast<float>(x) + 0.5f) * vs;
                float wy = (static_cast<float>(y) + 0.5f) * vs;
                float wz = (static_cast<float>(z) + 0.5f) * vs;
                if (blob(wx, wy, wz) >= 0.0f) continue;
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
    check_digest(g.mesh_greedy(), 24, 36, 0x1d743d89f9321907ull);
}

TEST_CASE("mesh_greedy fixture: single cell in negative space") {
    // fdiv/fmod_pos territory: chunk key -1, in-chunk offset 31 on every axis.
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({-1, -1, -1}, pal[1]);
    check_digest(g.mesh_greedy(), 24, 36, 0x52f6c574c397be37ull);
}

TEST_CASE("mesh_greedy fixture: two cells far apart") {
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({0, 0, 0}, pal[0]);
    g.set({2048, 2048, 2048}, pal[2]);
    check_digest(g.mesh_greedy(), 48, 72, 0xfaf5bbb7a26f7623ull);
}

TEST_CASE("mesh_greedy fixture: two cells far apart across the origin") {
    VoxelGrid g(0.1f);
    auto pal = seed_palette(g);
    g.set({-2048, -33, 5}, pal[3]);
    g.set({2048, 2048, -2048}, pal[4]);
    check_digest(g.mesh_greedy(), 48, 72, 0xa6a4becdccfc0ac3ull);
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
    check_digest(g.mesh_greedy(), 336, 504, 0x278d9f5a83d5a52bull);
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
    check_digest(g.mesh_greedy(), 2736, 4104, 0xea07eb4f037e2901ull);
}

TEST_CASE("mesh_greedy fixture: rasterized blob in the positive octant") {
    VoxelGrid g(0.05f);
    auto pal = seed_palette(g);
    fill_blob(g, 0.05f, pal, 40);
    check_digest(g.mesh_greedy(), 17212, 25818, 0xa86a0b4d978d601aull);
}

TEST_CASE("mesh_greedy fixture: rasterized blob straddling the origin") {
    VoxelGrid g(0.05f);
    auto pal = seed_palette(g);
    fill_blob(g, 0.05f, pal, 0);
    check_digest(g.mesh_greedy(), 17212, 25818, 0x3fbfb23e31b32b2aull);
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

    check_digest(g.mesh_greedy(0), 164, 246, 0x147ffa3b04a8f4c3ull);
    check_digest(g.mesh_greedy(1), 500, 750, 0x1c17324b076514deull);
    check_digest(g.mesh_greedy(2), 1496, 2244, 0x41c5ad117c745ce8ull);
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
    check_digest(g.mesh_greedy(), 1536, 2304, 0xfd958a27650da412ull);
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
