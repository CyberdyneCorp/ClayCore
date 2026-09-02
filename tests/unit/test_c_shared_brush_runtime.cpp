// THE SHARED BRUSH RUNTIME ACROSS THE C ABI (c-abi spec,
// add-shared-brush-runtime 4.6, 6.x).
//
// THE CONTRACT IS WRITTEN DOWN IN `clay.h`, WHICH IS WHY IT IS GATED HERE.
// `clay_dynamic_sculptor_stamp`'s own header says `brush` is "the same
// descriptor the fixed path takes, so a host carries one brush model across
// both representations". That sentence was false for the four automask fields:
// the descriptor carried them, `read_mesh_brush` decoded them, and
// `DynamicSculptor::gather` never read them — so a host that set
// `automask_factors` on an adaptive stamp got exactly the stamp it would have
// got with zero, and no error to say so. A C++ test can assert the behaviour;
// only a C test asserts the PROMISE, which is what a host reads.
//
// The rest of this file is the new ABI surface's refusals. Every descriptor in
// this library is versioned by `struct_size`, and the three ways that goes
// wrong — a size below the original layout, a size that is not a descriptor
// size at all, and a host that legitimately declares the SHORTER layout because
// it compiled against minor 74 — are each a different outcome. A test that only
// exercised the happy path would leave the ABI's only real safety mechanism
// ungated.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

// A cube-sphere as flat arrays, built through the C surface only — the same
// fixture `test_c_dynamic_topology.cpp` uses, for the same reason: a test of
// the ABI must not reach into C++ to build its inputs.
void cube_sphere(int n, float radius, std::vector<float>* positions,
                 std::vector<uint32_t>* indices) {
    positions->clear();
    indices->clear();
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = static_cast<uint32_t>(positions->size() / 3);
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                for (int k = 0; k < 3; ++k) positions->push_back(c[k] / len * radius);
            }
        const uint32_t stride = static_cast<uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const uint32_t a =
                    base + static_cast<uint32_t>(v) * stride + static_cast<uint32_t>(u);
                const uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    indices->insert(indices->end(), {a, c2, b, b, c2, d});
                else
                    indices->insert(indices->end(), {a, b, c2, b, d, c2});
            }
    }
}

struct AdaptiveFixture {
    clay_mesh* mesh = nullptr;
    clay_dynamic_surface* surface = nullptr;
    clay_dynamic_sculptor* sculptor = nullptr;

    explicit AdaptiveFixture(int n = 10) {
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        cube_sphere(n, 1.0f, &positions, &indices);
        REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                         indices.size(), &mesh) == CLAY_OK);
        int32_t err = -1;
        REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
        REQUIRE(err == CLAY_DYNAMIC_OK);
        REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
    }
    ~AdaptiveFixture() {
        clay_dynamic_sculptor_destroy(sculptor);
        clay_dynamic_surface_destroy(surface);
        clay_mesh_destroy(mesh);
    }
};

clay_dynamic_topology_desc topology_off() {
    clay_dynamic_topology_desc t{};
    t.struct_size = sizeof(t);
    REQUIRE(clay_dynamic_topology_defaults(&t) == CLAY_OK);
    t.enabled = 0;
    return t;
}

// The grab that the regression case below stamps: a fixed direction and a
// Euclidean footprint, so nothing about the result depends on a walk.
clay_mesh_brush_desc grab_brush() {
    clay_mesh_brush_desc b{};
    b.struct_size = sizeof(b);
    REQUIRE(clay_mesh_brush_defaults(&b) == CLAY_OK);
    b.verb = CLAY_MESH_BRUSH_GRAB;
    b.center[0] = 0.0f;
    b.center[1] = 0.0f;
    b.center[2] = 1.0f;
    b.radius = 1.6f;
    b.strength = 0.4f;
    b.geodesic = 0;
    b.direction[0] = 0.0f;
    b.direction[1] = 0.0f;
    b.direction[2] = 0.125f;
    return b;
}

clay_dynamic_stamp_report stamp(clay_dynamic_sculptor* sculptor,
                                const clay_mesh_brush_desc& brush) {
    clay_dynamic_topology_desc topology = topology_off();
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_dynamic_sculptor_stamp(sculptor, &brush, &topology, nullptr, &report) == CLAY_OK);
    return report;
}

}  // namespace

// -- 4.6: the regression, at the boundary where the promise is written --------

TEST_CASE("C ABI REGRESSION: an automasked adaptive stamp differs from an unmasked one") {
    // BEFORE THIS CHANGE THESE TWO CALLS RETURNED THE SAME REPORT. That is the
    // whole defect: `clay_mesh_brush_desc` carried `automask_factors` to
    // `clay_dynamic_sculptor_stamp`, `read_mesh_brush` decoded it into
    // `MeshBrushSettings::automask`, and the adaptive gather dropped it on the
    // floor — silently, with a CLAY_OK and a plausible-looking report.
    //
    // The angle is tightened to 0.5 radians rather than left at the default,
    // for the reason the C++ regression records: the factor is full strength up
    // to the angle and zero at TWICE it, so at the 60-degree default nothing on
    // a unit sphere under this brush turns far enough away to reach zero and
    // both calls legitimately report the same count.
    clay_mesh_brush_desc open = grab_brush();
    clay_mesh_brush_desc masked = grab_brush();
    masked.automask_factors = CLAY_AUTOMASK_NORMAL_ANGLE;
    masked.automask_normal_angle = 0.5f;

    uint64_t moved_open = 0, moved_masked = 0;
    {
        AdaptiveFixture f;
        moved_open = stamp(f.sculptor, open).moved_vertices;
    }
    {
        AdaptiveFixture f;
        moved_masked = stamp(f.sculptor, masked).moved_vertices;
    }

    CAPTURE(moved_open);
    CAPTURE(moved_masked);
    CHECK(moved_open == 365);
    CHECK(moved_masked == 149);
    CHECK(moved_masked < moved_open);
}

TEST_CASE("C ABI: the adaptive automask reaches the same set the fixed one does") {
    // The positive half of "the same descriptor the fixed path takes". One
    // descriptor, two sculptors, the same two numbers — which is the sentence
    // `clay_dynamic_sculptor_stamp`'s header makes and could not previously
    // keep.
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    cube_sphere(10, 1.0f, &positions, &indices);

    auto fixed_moved = [&](uint32_t factors) {
        clay_mesh* mesh = nullptr;
        REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                         indices.size(), &mesh) == CLAY_OK);
        clay_mesh_sculptor* sculptor = nullptr;
        // The weld epsilon the cube-sphere's six face grids need: its seams are
        // coincident duplicates, and the fixed sculptor is the representation
        // that has to rejoin them itself.
        REQUIRE(clay_mesh_sculptor_create(mesh, 1e-5f, &sculptor) == CLAY_OK);

        clay_mesh_brush_desc b = grab_brush();
        b.automask_factors = factors;
        b.automask_normal_angle = 0.5f;
        size_t moved = 0;
        REQUIRE(clay_mesh_sculptor_stamp(sculptor, &b, nullptr, nullptr, &moved) == CLAY_OK);

        clay_mesh_sculptor_destroy(sculptor);
        clay_mesh_destroy(mesh);
        return static_cast<uint64_t>(moved);
    };

    auto adaptive_moved = [&](uint32_t factors) {
        AdaptiveFixture f;
        clay_mesh_brush_desc b = grab_brush();
        b.automask_factors = factors;
        b.automask_normal_angle = 0.5f;
        return stamp(f.sculptor, b).moved_vertices;
    };

    CHECK(fixed_moved(0) == adaptive_moved(0));
    CHECK(fixed_moved(CLAY_AUTOMASK_NORMAL_ANGLE) == adaptive_moved(CLAY_AUTOMASK_NORMAL_ANGLE));
    CHECK(fixed_moved(CLAY_AUTOMASK_NORMAL_ANGLE) == 149);
}

// -- the appended azimuth, under the struct_size rule -------------------------

TEST_CASE("C ABI: an ABI-74 host declares the shorter layout and is unaffected") {
    // THE RULE THE WHOLE DESCRIPTOR SCHEME RESTS ON. A host compiled against
    // minor 74 has no `stamp_azimuth` field and declares a `struct_size` that
    // stops before it; the library must read the fields it does have and treat
    // the rest as absent, and it must write back no more than the host declared.
    //
    // `offsetof(stamp_azimuth)` is exactly what that older `sizeof` was: the
    // struct's tail is 8-byte aligned because of its pointer member, so the
    // field sits at 184 and minor 77's size was 192 — it grew by eight, not four.
    const uint32_t abi74 =
        static_cast<uint32_t>(offsetof(clay_mesh_brush_desc, stamp_azimuth));
    CHECK(abi74 == 184u);

    // AND THE SAME RULE ONE MINOR LATER. add-extreme-poly-runtime appends
    // `seed_revision` at minor 78, so the offset above is now TWO older layouts
    // back and the size is 200: `stamp_azimuth` is a float at 184, and a
    // uint64_t cannot start at 188, so the tail pads to 192 before it.
    //
    // Both offsets are asserted rather than just the size. A size check alone
    // passes if a field is INSERTED and another removed, which is the one edit
    // that silently breaks every host already compiled against this header.
    const uint32_t abi77 =
        static_cast<uint32_t>(offsetof(clay_mesh_brush_desc, seed_revision));
    CHECK(abi77 == 192u);
    CHECK(sizeof(clay_mesh_brush_desc) == 200u);

    clay_mesh_brush_desc b{};
    b.struct_size = sizeof(b);
    REQUIRE(clay_mesh_brush_defaults(&b) == CLAY_OK);
    // THE DEFAULT IS AN EXACT ZERO, which is the identity path: `make_stamp_frame`
    // branches on precisely this value, so a default that was 0.0f-but-negative
    // or an epsilon would take the rotation path on every stamp in the library.
    CHECK(b.stamp_azimuth == 0.0f);
    CHECK(std::signbit(b.stamp_azimuth) == false);

    // A host that declares the SHORTER layout still gets its defaults, and gets
    // to keep the size it declared — the library must not tell it that a field
    // it has no storage for exists.
    clay_mesh_brush_desc shorter{};
    shorter.struct_size = abi74;
    REQUIRE(clay_mesh_brush_defaults(&shorter) == CLAY_OK);
    CHECK(shorter.struct_size == abi74);
    CHECK(shorter.radius == b.radius);
    CHECK(shorter.strength == b.strength);

    // ...and a stamp taking that shorter descriptor behaves exactly as it did
    // at minor 74. Asserted against the full descriptor with an explicit zero
    // azimuth, which is the same stamp by construction.
    clay_mesh_brush_desc old_host = grab_brush();
    old_host.struct_size = abi74;
    clay_mesh_brush_desc new_host = grab_brush();
    new_host.stamp_azimuth = 0.0f;

    uint64_t a = 0, c = 0;
    {
        AdaptiveFixture f;
        a = stamp(f.sculptor, old_host).moved_vertices;
    }
    {
        AdaptiveFixture f;
        c = stamp(f.sculptor, new_host).moved_vertices;
    }
    CHECK(a == c);
    CHECK(a == 365);
}

TEST_CASE("C ABI: a malformed struct_size is refused rather than read past") {
    // THE THREE REFUSALS. `struct_size` is required — there is no "zero means
    // the original layout" sentinel, because a descriptor from an ABI that had
    // no `struct_size` puts a real field in that word.
    AdaptiveFixture f;
    clay_dynamic_topology_desc topology = topology_off();
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);

    clay_mesh_brush_desc b = grab_brush();

    b.struct_size = 0;
    CHECK(clay_dynamic_sculptor_stamp(f.sculptor, &b, &topology, nullptr, &report) !=
          CLAY_OK);

    b.struct_size = 8;
    CHECK(clay_dynamic_sculptor_stamp(f.sculptor, &b, &topology, nullptr, &report) !=
          CLAY_OK);

    // ABSURDLY LARGE, which is the direction that would be a buffer overrun
    // rather than a short read if it were honoured.
    b.struct_size = 100000;
    CHECK(clay_dynamic_sculptor_stamp(f.sculptor, &b, &topology, nullptr, &report) !=
          CLAY_OK);

    // ...and the well-formed one still works, so the refusals above are not the
    // call failing for some unrelated reason.
    b.struct_size = sizeof(b);
    CHECK(clay_dynamic_sculptor_stamp(f.sculptor, &b, &topology, nullptr, &report) == CLAY_OK);
}

// -- the arena statistics ------------------------------------------------------

TEST_CASE("C ABI: the arena statistics refuse a null sculptor rather than answering zeroes") {
    // A ZEROED ANSWER WOULD BE THE WORST OUTCOME AVAILABLE. A host budgeting
    // memory against these numbers reads "this sculptor's scratch cost nothing"
    // where the truth is "you passed me nothing", and the two are
    // indistinguishable at the call site.
    clay_brush_arena_stats stats{};
    stats.struct_size = sizeof(stats);
    CHECK(clay_mesh_sculptor_arena_stats(nullptr, &stats) != CLAY_OK);
    CHECK(clay_dynamic_sculptor_arena_stats(nullptr, &stats) != CLAY_OK);
    CHECK(clay_multires_sculptor_arena_stats(nullptr, &stats) != CLAY_OK);

    AdaptiveFixture f;
    CHECK(clay_dynamic_sculptor_arena_stats(f.sculptor, nullptr) != CLAY_OK);

    // And the same `struct_size` discipline the descriptors have.
    clay_brush_arena_stats bad{};
    bad.struct_size = 0;
    CHECK(clay_dynamic_sculptor_arena_stats(f.sculptor, &bad) != CLAY_OK);
    bad.struct_size = 4;
    CHECK(clay_dynamic_sculptor_arena_stats(f.sculptor, &bad) != CLAY_OK);
    bad.struct_size = 100000;
    CHECK(clay_dynamic_sculptor_arena_stats(f.sculptor, &bad) != CLAY_OK);
}

TEST_CASE("C ABI: the adaptive arena reports growth, and then reports convergence") {
    AdaptiveFixture f;

    clay_brush_arena_stats before{};
    before.struct_size = sizeof(before);
    REQUIRE(clay_dynamic_sculptor_arena_stats(f.sculptor, &before) == CLAY_OK);
    // NOTHING BEFORE THE FIRST STAMP. An arena that reported a reserve here
    // would be charging a host for storage no stroke had asked for.
    CHECK(before.capacity_bytes == 0);
    CHECK(before.high_water_bytes == 0);
    CHECK(before.growths == 0);

    clay_mesh_brush_desc b = grab_brush();
    b.radius = 0.4f;
    b.strength = 0.05f;

    for (int i = 0; i < 8; ++i) {
        b.center[0] = 0.03125f * static_cast<float>(i % 4);
        stamp(f.sculptor, b);
    }

    clay_brush_arena_stats warm{};
    warm.struct_size = sizeof(warm);
    REQUIRE(clay_dynamic_sculptor_arena_stats(f.sculptor, &warm) == CLAY_OK);
    CHECK(warm.capacity_bytes > 0);
    CHECK(warm.high_water_bytes > 0);
    CHECK(warm.growths > 0);
    // The high water is what ONE stamp peaked at, so it cannot exceed what the
    // arena owns.
    CHECK(warm.high_water_bytes <= warm.capacity_bytes);

    for (int i = 0; i < 40; ++i) {
        b.center[0] = 0.03125f * static_cast<float>(i % 4);
        stamp(f.sculptor, b);
    }

    clay_brush_arena_stats settled{};
    settled.struct_size = sizeof(settled);
    REQUIRE(clay_dynamic_sculptor_arena_stats(f.sculptor, &settled) == CLAY_OK);
    // THE NUMBER A HOST WATCHES. Forty more stamps of the same footprint took
    // no more storage, which is the arena having converged rather than leaking
    // a little per dab — the failure an allocation count alone cannot see.
    CAPTURE(warm.growths);
    CAPTURE(settled.growths);
    CHECK(settled.growths == warm.growths);
    CHECK(settled.capacity_bytes == warm.capacity_bytes);
}

TEST_CASE("C ABI: an arena reading is written back bounded by the size the host declared") {
    // The `write_desc` rule, which is a buffer overrun when it is got wrong and
    // was a real segfault the last time a descriptor grew. A host declaring the
    // ORIGINAL layout must have its trailing bytes left alone.
    AdaptiveFixture f;
    clay_mesh_brush_desc b = grab_brush();
    b.radius = 0.4f;
    stamp(f.sculptor, b);

    struct Guarded {
        clay_brush_arena_stats stats;
        uint64_t canary;
    };
    Guarded g{};
    g.stats.struct_size =
        static_cast<uint32_t>(offsetof(clay_brush_arena_stats, growths) + sizeof(uint64_t));
    g.canary = 0xfeedfacecafebeefull;

    REQUIRE(clay_dynamic_sculptor_arena_stats(f.sculptor, &g.stats) == CLAY_OK);
    CHECK(g.canary == 0xfeedfacecafebeefull);
    // The caller keeps the size THEY declared: it describes their buffer.
    CHECK(g.stats.struct_size ==
          static_cast<uint32_t>(offsetof(clay_brush_arena_stats, growths) + sizeof(uint64_t)));
    CHECK(g.stats.capacity_bytes > 0);
}

TEST_CASE("C ABI: the hierarchy's arena reads zero until a level is bound") {
    // NOT A PLACEHOLDER, THE TRUTH. A multiresolution stamp runs the fixed
    // sculptor over the active level's own mesh, so that is the arena that
    // grows; before the first stamp there is no bound level and nothing has
    // been spent. Reporting a second, always-empty arena of the hierarchy's own
    // would tell a host its scratch cost nothing forever.
    // A quad grid on the XZ plane, handed over as triangles — the same way
    // `test_c_multires.cpp` builds one, because the C surface takes triangles
    // and the hierarchy recovers the quads.
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    const int n = 4;
    const float step = 1.0f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            positions.push_back(-2.0f + step * static_cast<float>(x));
            positions.push_back(0.0f);
            positions.push_back(-2.0f + step * static_cast<float>(z));
        }
    const uint32_t stride = static_cast<uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = static_cast<uint32_t>(z) * stride + static_cast<uint32_t>(x);
            const uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            indices.insert(indices.end(), {a, b, c, a, c, d});
        }

    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &mesh) == CLAY_OK);

    clay_multires* surface = nullptr;
    int32_t err = -1;
    REQUIRE(clay_multires_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    REQUIRE(err == CLAY_MULTIRES_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);

    clay_multires_sculptor* sculptor = nullptr;
    REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);

    clay_brush_arena_stats stats{};
    stats.struct_size = sizeof(stats);
    REQUIRE(clay_multires_sculptor_arena_stats(sculptor, &stats) == CLAY_OK);
    CHECK(stats.capacity_bytes == 0);
    CHECK(stats.high_water_bytes == 0);
    CHECK(stats.growths == 0);

    // Bind a level and stamp with a factor that actually makes the arena work:
    // `Boundary`'s frontiers are the automask's only allocation on a surface
    // whose region is already one component, so a stamp without it would leave
    // the arena empty and this case would prove nothing.
    REQUIRE(clay_multires_set_sculpt_level(surface, 1) == CLAY_OK);
    clay_mesh_brush_desc b{};
    b.struct_size = sizeof(b);
    REQUIRE(clay_mesh_brush_defaults(&b) == CLAY_OK);
    b.verb = CLAY_MESH_BRUSH_GRAB;
    b.radius = 1.0f;
    b.strength = 0.25f;
    b.geodesic = 0;
    b.direction[1] = 0.125f;
    b.automask_factors = CLAY_AUTOMASK_BOUNDARY;

    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_multires_sculptor_stamp(sculptor, &b, nullptr, &report) == CLAY_OK);
    REQUIRE(report.moved_vertices > 0);

    clay_brush_arena_stats after{};
    after.struct_size = sizeof(after);
    REQUIRE(clay_multires_sculptor_arena_stats(sculptor, &after) == CLAY_OK);
    CHECK(after.capacity_bytes > 0);
    CHECK(after.growths > 0);

    clay_multires_sculptor_destroy(sculptor);
    clay_multires_destroy(surface);
    clay_mesh_destroy(mesh);
}
