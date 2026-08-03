#include <doctest/doctest.h>

#include <map>
#include <set>

#include "clay/voxel/grid.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

TEST_CASE("set/get/erase/paint and chunk reclamation") {
    VoxelGrid g(0.1f);
    std::uint8_t red = g.palette_add(cf3(1, 0, 0));
    std::uint8_t blue = g.palette_add(cf3(0, 0, 1));

    g.set({0, 0, 0}, red);
    g.set({-5, 3, 100}, blue);  // negative and cross-chunk coords
    CHECK(g.get({0, 0, 0}) == red);
    CHECK(g.get({-5, 3, 100}) == blue);
    CHECK(g.get({1, 0, 0}) == 0);
    CHECK(g.occupied_count() == 2);

    // paint only affects occupied cells
    g.paint({1, 0, 0}, red);
    CHECK(g.get({1, 0, 0}) == 0);
    g.paint({0, 0, 0}, blue);
    CHECK(g.get({0, 0, 0}) == blue);

    g.erase({0, 0, 0});
    g.erase({-5, 3, 100});
    CHECK(g.occupied_count() == 0);
}

TEST_CASE("palette: dedupe, recolor without touching voxels") {
    VoxelGrid g(0.1f);
    std::uint8_t a = g.palette_add(cf3(1, 0, 0));
    std::uint8_t b = g.palette_add(cf3(1, 0, 0));  // same color -> same index
    CHECK(a == b);
    g.set({0, 0, 0}, a);
    g.palette_set(a, cf3(0, 1, 0));  // recolor palette entry
    CHECK(g.get({0, 0, 0}) == a);    // voxel data untouched
    CHECK(g.palette_color(a).y == doctest::Approx(1.0f));
}

TEST_CASE("brush, box fill, line fill") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(0.5f, 0.5f, 0.5f));
    g.set_brush({0, 0, 0}, 3, c);
    CHECK(g.occupied_count() == 27);
    CHECK(g.get({1, 1, 1}) == c);
    CHECK(g.get({2, 0, 0}) == 0);
    g.erase_brush({0, 0, 0}, 3);
    CHECK(g.occupied_count() == 0);

    g.fill_box({0, 0, 0}, {4, 2, 1}, c);
    CHECK(g.occupied_count() == 5 * 3 * 2);

    VoxelGrid g2(0.1f);
    g2.fill_line({0, 0, 0}, {10, 5, 2}, c);
    CHECK(g2.get({0, 0, 0}) == c);
    CHECK(g2.get({10, 5, 2}) == c);
    CHECK(g2.occupied_count() >= 11);  // at least one voxel per major-axis step
}

TEST_CASE("mirrored edits land on all mirror combinations") {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 0));
    g.set_mirrored({3, 2, 1}, c, voxel::kVoxMirrorX);
    CHECK(g.get({3, 2, 1}) == c);
    CHECK(g.get({-4, 2, 1}) == c);  // x -> -1-x
    CHECK(g.occupied_count() == 2);

    VoxelGrid g2(0.1f);
    g2.set_mirrored({3, 2, 1}, c, voxel::kVoxMirrorX | voxel::kVoxMirrorZ);
    CHECK(g2.occupied_count() == 4);  // all four combinations
    CHECK(g2.get({-4, 2, -2}) == c);
}

TEST_CASE("build-plane pick resolves the cell under a ray") {
    VoxelGrid g(0.5f);
    math::Ray ray{cf3(1.3f, 5.0f, 0.8f), cnormalize(cf3(0, -1.0f, 0))};
    auto cell = g.build_plane_pick(ray, 0);
    REQUIRE(cell.has_value());
    CHECK(cell->x == 2);  // 1.3 / 0.5
    CHECK(cell->y == 0);
    CHECK(cell->z == 1);  // 0.8 / 0.5
    // ray parallel to the plane
    CHECK_FALSE(g.build_plane_pick({cf3(0, 1, 0), cf3(1, 0, 0)}, 0).has_value());
}

TEST_CASE("flood select: connectivity and color modes") {
    VoxelGrid g(0.1f);
    std::uint8_t red = g.palette_add(cf3(1, 0, 0));
    std::uint8_t blue = g.palette_add(cf3(0, 0, 1));
    // two red bars connected by a blue bridge
    g.fill_box({0, 0, 0}, {3, 0, 0}, red);
    g.set({4, 0, 0}, blue);
    g.fill_box({5, 0, 0}, {8, 0, 0}, red);
    // disconnected red block
    g.set({0, 5, 0}, red);

    CHECK(g.flood_select({0, 0, 0}, true).size() == 4);    // stops at the blue bridge
    CHECK(g.flood_select({0, 0, 0}, false).size() == 9);   // crosses it
    CHECK(g.flood_select({0, 5, 0}, true).size() == 1);
    CHECK(g.flood_select({2, 5, 0}, true).empty());        // empty seed
}

TEST_CASE("serialization: palette+RLE round trip is lossless and canonical") {
    VoxelGrid g(0.25f);
    clay_test::Lcg rng(701);
    std::uint8_t idx[3] = {g.palette_add(cf3(1, 0, 0)), g.palette_add(cf3(0, 1, 0)),
                           g.palette_add(cf3(0, 0, 1))};
    for (int i = 0; i < 500; ++i) {
        VoxelCoord c{static_cast<int>(rng.range(-40, 40)), static_cast<int>(rng.range(-40, 40)),
                     static_cast<int>(rng.range(-40, 40))};
        g.set(c, idx[i % 3]);
    }
    std::vector<std::uint8_t> bytes = g.serialize();
    auto back = VoxelGrid::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(back->occupied_count() == g.occupied_count());
    CHECK(back->voxel_size() == g.voxel_size());
    CHECK(back->serialize() == bytes);  // canonical

    // sparse efficiency: RLE beats dense chunk dumps by a wide margin
    CHECK(bytes.size() < 500 * 32);

    // truncated input rejected
    for (std::size_t cut : {std::size_t(0), bytes.size() / 2, bytes.size() - 1})
        CHECK_FALSE(VoxelGrid::deserialize(bytes.data(), cut).has_value());
}

TEST_CASE("greedy meshing is lossless: exposed faces covered exactly once") {
    VoxelGrid g(1.0f);
    std::uint8_t red = g.palette_add(cf3(1, 0, 0));
    std::uint8_t blue = g.palette_add(cf3(0, 0, 1));
    // an L-shaped bi-color solid exercises merging and color boundaries
    g.fill_box({0, 0, 0}, {5, 2, 3}, red);
    g.fill_box({0, 3, 0}, {2, 5, 3}, blue);

    mesh::Mesh m = g.mesh_greedy();
    REQUIRE(!m.empty());
    REQUIRE(m.normals.size() == m.positions.size());
    REQUIRE(m.colors.size() == m.positions.size());

    // face key: (axis, sign, plane coordinate, cell u, cell v)
    using FaceKey = std::tuple<int, int, int, int, int>;

    // brute-force expected exposed faces
    std::map<FaceKey, cfloat3> expected;
    auto b0 = g.bounds_min().value();
    auto b1 = g.bounds_max().value();
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int z = b0.z; z <= b1.z; ++z)
        for (int y = b0.y; y <= b1.y; ++y)
            for (int x = b0.x; x <= b1.x; ++x) {
                std::uint8_t v = g.get({x, y, z});
                if (v == 0) continue;
                for (const auto& d : dirs) {
                    if (g.get({x + d[0], y + d[1], z + d[2]}) != 0) continue;
                    int axis = d[0] != 0 ? 0 : (d[1] != 0 ? 1 : 2);
                    int sign = d[axis] > 0 ? 1 : -1;
                    int cell[3] = {x, y, z};
                    int plane = cell[axis] + (sign > 0 ? 1 : 0);
                    int u = cell[(axis + 1) % 3], w = cell[(axis + 2) % 3];
                    expected[{axis, sign, plane, u, w}] = g.palette_color(v);
                }
            }

    // clip each (axis-aligned) triangle against every unit square in its
    // plane and accumulate exact overlap area per face
    auto clip_area = [](cfloat2 t0, cfloat2 t1, cfloat2 t2, float cu, float cv) {
        std::vector<cfloat2> poly = {t0, t1, t2};
        auto clip_lo = [&](int comp, float bound) {
            std::vector<cfloat2> out;
            for (std::size_t i = 0; i < poly.size(); ++i) {
                cfloat2 a = poly[i], b = poly[(i + 1) % poly.size()];
                float av = comp == 0 ? a.x : a.y, bv = comp == 0 ? b.x : b.y;
                bool ain = av >= bound, bin = bv >= bound;
                if (ain) out.push_back(a);
                if (ain != bin) {
                    float t = (bound - av) / (bv - av);
                    out.push_back(a + (b - a) * t);
                }
            }
            poly = out;
        };
        auto clip_hi = [&](int comp, float bound) {
            std::vector<cfloat2> out;
            for (std::size_t i = 0; i < poly.size(); ++i) {
                cfloat2 a = poly[i], b = poly[(i + 1) % poly.size()];
                float av = comp == 0 ? a.x : a.y, bv = comp == 0 ? b.x : b.y;
                bool ain = av <= bound, bin = bv <= bound;
                if (ain) out.push_back(a);
                if (ain != bin) {
                    float t = (bound - av) / (bv - av);
                    out.push_back(a + (b - a) * t);
                }
            }
            poly = out;
        };
        clip_lo(0, cu);
        clip_hi(0, cu + 1.0f);
        clip_lo(1, cv);
        clip_hi(1, cv + 1.0f);
        float area = 0.0f;
        for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
            cfloat2 e0 = poly[i] - poly[0];
            cfloat2 e1 = poly[i + 1] - poly[0];
            area += 0.5f * (e0.x * e1.y - e0.y * e1.x);
        }
        return cabs(area);
    };

    std::map<FaceKey, float> covered;
    std::map<FaceKey, cfloat3> covered_color;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        cfloat3 p[3] = {m.positions[m.indices[t * 3]], m.positions[m.indices[t * 3 + 1]],
                        m.positions[m.indices[t * 3 + 2]]};
        cfloat3 nrm = m.normals[m.indices[t * 3]];
        int axis = cabs(nrm.x) > 0.5f ? 0 : (cabs(nrm.y) > 0.5f ? 1 : 2);
        float naxis = axis == 0 ? nrm.x : (axis == 1 ? nrm.y : nrm.z);
        int sign = naxis > 0 ? 1 : -1;
        auto comp = [](cfloat3 v, int c) { return c == 0 ? v.x : (c == 1 ? v.y : v.z); };
        int plane = static_cast<int>(std::lround(comp(p[0], axis)));
        int ua = (axis + 1) % 3, va = (axis + 2) % 3;
        cfloat2 t2d[3];
        for (int i = 0; i < 3; ++i) t2d[i] = cf2(comp(p[i], ua), comp(p[i], va));
        cfloat2 lo = cmin(cmin(t2d[0], t2d[1]), t2d[2]);
        cfloat2 hi = cmax(cmax(t2d[0], t2d[1]), t2d[2]);
        for (int cv = static_cast<int>(std::floor(lo.y)); cv < static_cast<int>(std::ceil(hi.y)); ++cv)
            for (int cu = static_cast<int>(std::floor(lo.x)); cu < static_cast<int>(std::ceil(hi.x)); ++cu) {
                float area = clip_area(t2d[0], t2d[1], t2d[2], static_cast<float>(cu),
                                       static_cast<float>(cv));
                if (area < 1e-6f) continue;
                FaceKey key{axis, sign, plane, cu, cv};
                covered[key] += area;
                covered_color[key] = m.colors[m.indices[t * 3]];
            }
    }

    // every expected face covered with total area exactly 1, right color,
    // and nothing extra
    for (const auto& [key, color] : expected) {
        auto it = covered.find(key);
        REQUIRE(it != covered.end());
        CHECK(it->second == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(clength(covered_color[key] - color) < 1e-5f);
    }
    CHECK(covered.size() == expected.size());

    // merging actually reduced the face count
    CHECK(m.triangle_count() < expected.size() * 2);
}

TEST_CASE("step-function field bridges voxels into SDF compositing") {
    VoxelGrid g(0.5f);
    g.fill_box({0, 0, 0}, {1, 1, 1}, g.palette_add(cf3(1, 1, 1)));
    CHECK(g.sample_step_field(cf3(0.25f, 0.25f, 0.25f)) < 0.0f);   // inside a voxel
    CHECK(g.sample_step_field(cf3(-0.25f, 0.25f, 0.25f)) > 0.0f);  // outside
}

TEST_CASE("SDF rasterization: inside centers set with field colors") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
    n.color = cf3(0.9f, 0.1f, 0.2f);
    l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    VoxelGrid g(0.1f);
    g.rasterize_tape(tape, math::Aabb{cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f)});
    CHECK(g.occupied_count() > 0);
    // center voxel set, corner voxel not
    CHECK(g.get({0, 0, 0}) != 0);
    CHECK(g.get({6, 6, 6}) == 0);
    // occupancy matches the sphere volume within a coarse tolerance
    float volume = static_cast<float>(g.occupied_count()) * 0.1f * 0.1f * 0.1f;
    CHECK(volume == doctest::Approx(4.18879f * 0.125f).epsilon(0.15));
    // color came from the field
    CHECK(clength(g.palette_color(g.get({0, 0, 0})) - cf3(0.9f, 0.1f, 0.2f)) < 0.05f);
}
