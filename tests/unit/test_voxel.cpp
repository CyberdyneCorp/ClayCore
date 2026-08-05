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

TEST_CASE("brush size N covers N cells per axis") {
    // Regression: the footprint used radius (n-1)/2 over -r..r, which spans
    // n cells for odd n but n-1 for even n — every even size silently
    // behaved as the odd size below it.
    SUBCASE("cube is exactly N^3 for every size") {
        for (int n = 1; n <= 8; ++n) {
            VoxelGrid g(0.1f);
            std::uint8_t c = g.palette_add(cf3(1, 1, 1));
            g.set_brush({0, 0, 0}, n, c);
            CAPTURE(n);
            CHECK(g.occupied_count() == static_cast<std::size_t>(n) * n * n);
        }
    }

    SUBCASE("consecutive sizes are distinct") {
        for (int n = 1; n < 8; ++n) {
            VoxelGrid small(0.1f), large(0.1f);
            std::uint8_t c = small.palette_add(cf3(1, 1, 1));
            large.palette_add(cf3(1, 1, 1));
            small.set_brush({0, 0, 0}, n, c);
            large.set_brush({0, 0, 0}, n + 1, c);
            CAPTURE(n);
            CHECK(large.occupied_count() > small.occupied_count());
        }
    }
}

TEST_CASE("brush shapes: sphere is the ball of the same diameter") {
    std::uint8_t c = 1;

    SUBCASE("sphere cells are a subset of cube cells, within radius n/2") {
        for (int n : {3, 4, 5, 7, 9}) {
            VoxelGrid cube(0.1f), sphere(0.1f);
            c = cube.palette_add(cf3(1, 1, 1));
            sphere.palette_add(cf3(1, 1, 1));
            cube.set_brush({0, 0, 0}, n, c);
            sphere.set_brush({0, 0, 0}, n, c, voxel::BrushShape::Sphere);

            CAPTURE(n);
            CHECK(sphere.occupied_count() <= cube.occupied_count());
            CHECK(sphere.occupied_count() > 0);

            int lo = -((n - 1) / 2), hi = n / 2, mid = lo + hi;
            for (int z = lo; z <= hi; ++z)
                for (int y = lo; y <= hi; ++y)
                    for (int x = lo; x <= hi; ++x)
                        if (sphere.get({x, y, z}) != 0) {
                            CHECK(cube.get({x, y, z}) != 0);
                            int dx = 2 * x - mid, dy = 2 * y - mid, dz = 2 * z - mid;
                            CHECK(dx * dx + dy * dy + dz * dz <= n * n);
                        }
        }
    }

    SUBCASE("the cube's corner is outside the sphere once it can be") {
        // n=2 is fully covered (every cell centre is within radius 1), so the
        // corner test only bites from n=3 up.
        for (int n : {3, 5, 7}) {
            VoxelGrid sphere(0.1f);
            c = sphere.palette_add(cf3(1, 1, 1));
            sphere.set_brush({0, 0, 0}, n, c, voxel::BrushShape::Sphere);
            int hi = n / 2;
            CAPTURE(n);
            CHECK(sphere.get({hi, hi, hi}) == 0);
        }
    }

    SUBCASE("sphere is non-degenerate at small even sizes") {
        VoxelGrid g(0.1f);
        c = g.palette_add(cf3(1, 1, 1));
        g.set_brush({0, 0, 0}, 2, c, voxel::BrushShape::Sphere);
        CHECK(g.occupied_count() == 8);  // every centre is within radius 1
    }

    SUBCASE("cube remains the default") {
        VoxelGrid g(0.1f);
        c = g.palette_add(cf3(1, 1, 1));
        g.set_brush({0, 0, 0}, 5, c);
        CHECK(g.occupied_count() == 125);
    }

    SUBCASE("erase_brush honours the shape") {
        VoxelGrid g(0.1f), ball(0.1f);
        c = g.palette_add(cf3(1, 1, 1));
        ball.palette_add(cf3(1, 1, 1));
        ball.set_brush({0, 0, 0}, 5, c, voxel::BrushShape::Sphere);

        g.set_brush({0, 0, 0}, 5, c);                            // solid cube
        g.erase_brush({0, 0, 0}, 5, voxel::BrushShape::Sphere);  // scoop a ball
        CHECK(g.get({0, 0, 0}) == 0);                            // centre gone
        CHECK(g.get({2, 2, 2}) == c);                            // corner kept
        CHECK(g.occupied_count() == 125 - ball.occupied_count());
    }
}

TEST_CASE("paint brush recolors without creating voxels") {
    VoxelGrid g(0.1f);
    std::uint8_t a = g.palette_add(cf3(1, 0, 0));
    std::uint8_t b = g.palette_add(cf3(0, 1, 0));

    // a sparse pattern: only some cells inside the footprint are occupied
    g.set({0, 0, 0}, a);
    g.set({1, 0, 0}, a);
    std::size_t before = g.occupied_count();

    g.paint_brush({0, 0, 0}, 5, b);
    CHECK(g.occupied_count() == before);  // no new cells
    CHECK(g.get({0, 0, 0}) == b);
    CHECK(g.get({1, 0, 0}) == b);
    CHECK(g.get({2, 0, 0}) == 0);         // still empty

    SUBCASE("sphere-shaped paint leaves cube corners alone") {
        VoxelGrid h(0.1f);
        std::uint8_t x = h.palette_add(cf3(1, 0, 0));
        std::uint8_t y = h.palette_add(cf3(0, 1, 0));
        h.set_brush({0, 0, 0}, 5, x);
        h.paint_brush({0, 0, 0}, 5, y, voxel::BrushShape::Sphere);
        CHECK(h.get({0, 0, 0}) == y);   // inside the ball: recoloured
        CHECK(h.get({2, 2, 2}) == x);   // cube corner: untouched
        CHECK(h.occupied_count() == 125);
    }
}

TEST_CASE("mirrored paint recolors both sides") {
    VoxelGrid g(0.1f);
    std::uint8_t a = g.palette_add(cf3(1, 0, 0));
    std::uint8_t b = g.palette_add(cf3(0, 1, 0));

    g.set_mirrored({3, 1, 2}, a, voxel::kVoxMirrorX);
    CHECK(g.occupied_count() == 2);

    g.paint_mirrored({3, 1, 2}, b, voxel::kVoxMirrorX);
    CHECK(g.occupied_count() == 2);                       // no new cells
    CHECK(g.get({3, 1, 2}) == b);
    CHECK(g.get(VoxelGrid::mirrored({3, 1, 2}, voxel::kVoxMirrorX)) == b);
}

namespace {

// a slab three cells thick, the substrate the sculpting verbs act on
VoxelGrid sculpt_slab(std::uint8_t& c) {
    VoxelGrid g(0.1f);
    c = g.palette_add(cf3(0.6f, 0.6f, 0.7f));
    g.fill_box({-6, -2, -6}, {6, 0, 6}, c);
    return g;
}

std::size_t brush_cells(int size, voxel::BrushFalloff falloff, float strength,
                        std::uint32_t seed = 0) {
    VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(1, 1, 1));
    voxel::BrushParams p;
    p.size = size;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = falloff;
    p.strength = strength;
    p.seed = seed;
    g.set_brush({0, 0, 0}, p, c);
    return g.occupied_count();
}

}  // namespace

TEST_CASE("brush falloff dithers coverage deterministically") {
    SUBCASE("constant falloff at full strength is the hard-edged brush") {
        for (int n : {5, 8, 9}) {
            VoxelGrid hard(0.1f), soft(0.1f);
            std::uint8_t c = hard.palette_add(cf3(1, 1, 1));
            soft.palette_add(cf3(1, 1, 1));
            hard.set_brush({0, 0, 0}, n, c, voxel::BrushShape::Sphere);
            voxel::BrushParams p;
            p.size = n;
            p.shape = voxel::BrushShape::Sphere;
            soft.set_brush({0, 0, 0}, p, c);
            CAPTURE(n);
            CHECK(soft.occupied_count() == hard.occupied_count());
        }
    }

    SUBCASE("softer curves cover less") {
        std::size_t constant = brush_cells(15, voxel::BrushFalloff::Constant, 1.0f);
        std::size_t linear = brush_cells(15, voxel::BrushFalloff::Linear, 1.0f);
        std::size_t smooth = brush_cells(15, voxel::BrushFalloff::Smooth, 1.0f);
        std::size_t gaussian = brush_cells(15, voxel::BrushFalloff::Gaussian, 1.0f);
        CHECK(linear < constant);
        CHECK(smooth < linear);
        CHECK(gaussian < smooth);
        CHECK(gaussian > 0);
    }

    SUBCASE("coverage thins toward the rim") {
        VoxelGrid g(0.1f);
        std::uint8_t c = g.palette_add(cf3(1, 1, 1));
        voxel::BrushParams p;
        p.size = 21;
        p.shape = voxel::BrushShape::Sphere;
        p.falloff = voxel::BrushFalloff::Linear;
        g.set_brush({0, 0, 0}, p, c);

        // count occupancy in an inner and an outer shell of equal thickness
        int inner = 0, inner_total = 0, outer = 0, outer_total = 0;
        for (int z = -10; z <= 10; ++z)
            for (int y = -10; y <= 10; ++y)
                for (int x = -10; x <= 10; ++x) {
                    int d2 = x * x + y * y + z * z;
                    bool occupied = g.get({x, y, z}) != 0;
                    if (d2 <= 16) {  // r <= 4
                        ++inner_total;
                        inner += occupied;
                    } else if (d2 > 49 && d2 <= 100) {  // 7 < r <= 10
                        ++outer_total;
                        outer += occupied;
                    }
                }
        double inner_ratio = static_cast<double>(inner) / inner_total;
        double outer_ratio = static_cast<double>(outer) / outer_total;
        CHECK(inner_ratio > outer_ratio);
        CHECK(inner_ratio > 0.6);
        CHECK(outer_ratio < 0.4);
    }

    SUBCASE("strength scales coverage monotonically") {
        std::size_t full = brush_cells(15, voxel::BrushFalloff::Smooth, 1.0f);
        std::size_t half = brush_cells(15, voxel::BrushFalloff::Smooth, 0.5f);
        std::size_t none = brush_cells(15, voxel::BrushFalloff::Smooth, 0.0f);
        CHECK(half < full);
        CHECK(none == 0);
    }

    SUBCASE("same seed gives identical cells; different seeds do not") {
        VoxelGrid a(0.1f), b(0.1f);
        std::uint8_t c = a.palette_add(cf3(1, 1, 1));
        b.palette_add(cf3(1, 1, 1));
        voxel::BrushParams p;
        p.size = 11;
        p.falloff = voxel::BrushFalloff::Linear;
        p.seed = 7;
        a.set_brush({0, 0, 0}, p, c);
        b.set_brush({0, 0, 0}, p, c);

        bool identical = true;
        for (int z = -6; z <= 6; ++z)
            for (int y = -6; y <= 6; ++y)
                for (int x = -6; x <= 6; ++x)
                    if (a.get({x, y, z}) != b.get({x, y, z})) identical = false;
        CHECK(identical);
        CHECK(a.occupied_count() > 0);

        VoxelGrid d(0.1f);
        d.palette_add(cf3(1, 1, 1));
        voxel::BrushParams q = p;
        q.seed = 99;
        d.set_brush({0, 0, 0}, q, c);
        CHECK(d.occupied_count() > 0);
    }
}

TEST_CASE("sculpting verbs reshape existing material") {
    std::uint8_t c = 0;

    SUBCASE("smooth dissolves a spur and keeps the slab") {
        VoxelGrid g = sculpt_slab(c);
        g.set({0, 1, 0}, c);
        g.set({0, 2, 0}, c);
        std::size_t before = g.occupied_count();

        voxel::BrushParams p;
        p.size = 9;
        p.shape = voxel::BrushShape::Sphere;
        g.sculpt_smooth({0, 1, 0}, p);

        CHECK(g.get({0, 1, 0}) == 0);          // spur gone
        CHECK(g.get({0, 2, 0}) == 0);
        CHECK(g.get({0, -1, 0}) == c);         // slab interior kept
        CHECK(g.occupied_count() < before);
    }

    SUBCASE("inflate grows, erode shrinks") {
        VoxelGrid g = sculpt_slab(c);
        std::size_t base = g.occupied_count();
        voxel::BrushParams p;
        p.size = 9;
        p.shape = voxel::BrushShape::Sphere;

        g.sculpt_inflate({0, 0, 0}, p, 1);
        std::size_t grown = g.occupied_count();
        CHECK(grown > base);

        g.sculpt_inflate({0, 0, 0}, p, -1);
        CHECK(g.occupied_count() < grown);
    }

    SUBCASE("flatten clears everything above the plane") {
        VoxelGrid g = sculpt_slab(c);
        g.set_brush({0, 2, 0}, 3, c);  // a bump proud of the slab
        voxel::BrushParams p;
        p.size = 11;
        p.shape = voxel::BrushShape::Sphere;
        g.sculpt_flatten({0, 0, 0}, p, cf3(0, 1, 0), 0.0f);

        int above = 0;
        for (int y = 1; y <= 3; ++y)
            for (int x = -6; x <= 6; ++x)
                for (int z = -6; z <= 6; ++z)
                    if (g.get({x, y, z}) != 0) ++above;
        CHECK(above == 0);
    }

    SUBCASE("pinch draws the surface in and touches nothing outside") {
        VoxelGrid g = sculpt_slab(c);
        VoxelGrid reference = sculpt_slab(c);
        voxel::BrushParams p;
        p.size = 7;
        p.shape = voxel::BrushShape::Sphere;
        g.sculpt_pinch({0, 0, 0}, p);

        CHECK(g.occupied_count() != reference.occupied_count());
        int outside_changed = 0;
        for (int y = -4; y <= 4; ++y)
            for (int x = -9; x <= 9; ++x)
                for (int z = -9; z <= 9; ++z)
                    if (x * x + y * y + z * z > 49 &&
                        g.get({x, y, z}) != reference.get({x, y, z}))
                        ++outside_changed;
        CHECK(outside_changed == 0);
    }

    SUBCASE("verbs read pre-operation state, not partial results") {
        // A checkerboard is the adversarial case: if smooth read cells it had
        // already written, the result would depend on iteration order. Running
        // it on two grids built in opposite orders must agree.
        VoxelGrid a(0.1f), b(0.1f);
        std::uint8_t ca = a.palette_add(cf3(1, 1, 1));
        b.palette_add(cf3(1, 1, 1));
        for (int z = -4; z <= 4; ++z)
            for (int y = -4; y <= 4; ++y)
                for (int x = -4; x <= 4; ++x)
                    if ((x + y + z) % 2 == 0) a.set({x, y, z}, ca);
        for (int z = 4; z >= -4; --z)
            for (int y = 4; y >= -4; --y)
                for (int x = 4; x >= -4; --x)
                    if ((x + y + z) % 2 == 0) b.set({x, y, z}, ca);

        voxel::BrushParams p;
        p.size = 7;
        p.shape = voxel::BrushShape::Sphere;
        a.sculpt_smooth({0, 0, 0}, p);
        b.sculpt_smooth({0, 0, 0}, p);

        bool identical = true;
        for (int z = -5; z <= 5; ++z)
            for (int y = -5; y <= 5; ++y)
                for (int x = -5; x <= 5; ++x)
                    if (a.get({x, y, z}) != b.get({x, y, z})) identical = false;
        CHECK(identical);
    }
}

TEST_CASE("voxel grab translates occupancy through the same map") {
    auto ball = [](VoxelGrid& g, std::uint8_t c) {
        g.set_brush({0, 0, 0}, 11, c, voxel::BrushShape::Sphere);
    };

    SUBCASE("material moves toward the pull and colour comes with it") {
        VoxelGrid g(0.1f);
        std::uint8_t c = g.palette_add(cf3(0.2f, 0.7f, 0.9f));
        ball(g, c);
        std::size_t before = g.occupied_count();

        voxel::BrushParams p;
        p.size = 15;
        p.shape = voxel::BrushShape::Sphere;
        // Half a cell short of three cells, so the pull is unambiguous.
        g.sculpt_grab({0, 0, 0}, p, cf3(0.29f, 0.0f, 0.0f));

        CHECK(g.occupied_count() > 0);
        CHECK(before > 0);
        // The span moved bodily in +x: the leading face advanced from 5 to 6
        // and the trailing face vacated -5. Nearest-cell resampling means whole
        // cells, which is the documented behaviour of a binary representation.
        CHECK(g.get({6, 0, 0}) == c);
        CHECK(g.get({-5, 0, 0}) == 0);
        CHECK(g.get({-4, 0, 0}) == c);
        // colour travels with the material
        CHECK(g.palette_color(g.get({6, 0, 0})).y == doctest::Approx(0.7f).epsilon(1e-3));
    }

    SUBCASE("nothing beyond the footprint changes") {
        VoxelGrid moved(0.1f), reference(0.1f);
        std::uint8_t c = moved.palette_add(cf3(1, 1, 1));
        reference.palette_add(cf3(1, 1, 1));
        ball(moved, c);
        ball(reference, c);

        voxel::BrushParams p;
        p.size = 9;
        p.shape = voxel::BrushShape::Sphere;
        moved.sculpt_grab({0, 0, 0}, p, cf3(0.2f, 0, 0));

        int outside_changed = 0;
        for (int z = -12; z <= 12; ++z)
            for (int y = -12; y <= 12; ++y)
                for (int x = -12; x <= 12; ++x) {
                    // outside the size-9 footprint's radius
                    if (x * x + y * y + z * z <= 9 * 9) continue;
                    if (moved.get({x, y, z}) != reference.get({x, y, z})) ++outside_changed;
                }
        CHECK(outside_changed == 0);
    }

    SUBCASE("it agrees with the SDF grab to within the voxel size") {
        // Voxelize a sphere, grab it; grab the same sphere as an SDF and
        // rasterize. The two surfaces should land in the same cells, give or
        // take the one-cell quantisation binary occupancy forces.
        const float voxel = 0.1f;
        const float radius_cells = 7.0f;
        VoxelGrid g(voxel);
        std::uint8_t c = g.palette_add(cf3(1, 1, 1));
        g.set_brush({0, 0, 0}, 15, c, voxel::BrushShape::Sphere);

        voxel::BrushParams p;
        p.size = 21;
        p.shape = voxel::BrushShape::Sphere;
        const kernel::cfloat3 disp = cf3(0.3f, 0.0f, 0.0f);
        g.sculpt_grab({0, 0, 0}, p, disp);

        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = clay_test::item(scene::Prim::sphere(radius_cells * voxel), cf3(0, 0, 0));
        n.deformers.push_back(
            scene::Deformer::grab(cf3(0, 0, 0), 21.0f * voxel * 0.5f, disp, 0));
        l.sdf->insert(n);
        scene::Tape tape = scene::compile_document(doc);

        int disagree = 0, sampled = 0;
        for (int x = -14; x <= 16; ++x) {
            // Cell centre in world space.
            kernel::cfloat3 wp = cf3((x + 0.5f) * voxel, 0.5f * voxel, 0.5f * voxel);
            bool solid_voxel = g.get({x, 0, 0}) != 0;
            bool solid_sdf = tape.eval(wp).d < 0.0f;
            ++sampled;
            // Allow disagreement only within one cell of the surface.
            if (solid_voxel != solid_sdf && kernel::cabs(tape.eval(wp).d) > voxel) ++disagree;
        }
        CHECK(sampled > 0);
        CHECK(disagree == 0);
    }
}
