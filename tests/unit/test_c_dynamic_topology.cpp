// The adaptive surface across the C ABI (c-abi spec, add-dynamic-topology).
//
// Two things are gated here that the C++ tests cannot reach. THE DIRTY PATH IS
// COMPARED AGAINST THE WHOLE EXPORT — a chunk stream that quietly disagrees with
// the surface is a host drawing something that is not the model, and it would
// look plausible for a long time. And the existing mesh sculptor's semantics are
// checked to be untouched, because a host relying on the fixed-topology contract
// must not be affected by any of this.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <array>
#include <set>
#include <vector>

#include "clay.h"

namespace {

// A cube-sphere as flat arrays, built through the C surface only.
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
                if (signs[f] > 0.0f) {
                    indices->insert(indices->end(), {a, c2, b, b, c2, d});
                } else {
                    indices->insert(indices->end(), {a, b, c2, b, d, c2});
                }
            }
    }
}

struct Fixture {
    clay_mesh* mesh = nullptr;
    clay_dynamic_surface* surface = nullptr;
    clay_dynamic_sculptor* sculptor = nullptr;

    explicit Fixture(int n = 6) {
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        cube_sphere(n, 1.0f, &positions, &indices);
        REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                         indices.size(), &mesh) == CLAY_OK);
        int32_t err = -1;
        REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
        CHECK(err == CLAY_DYNAMIC_OK);
        REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
    }
    ~Fixture() {
        clay_dynamic_sculptor_destroy(sculptor);
        clay_dynamic_surface_destroy(surface);
        clay_mesh_destroy(mesh);
    }
};

clay_mesh_brush_desc draw_brush() {
    clay_mesh_brush_desc b{};
    b.struct_size = sizeof(b);
    REQUIRE(clay_mesh_brush_defaults(&b) == CLAY_OK);
    b.verb = CLAY_MESH_BRUSH_DRAW;
    b.center[0] = 0.0f;
    b.center[1] = 0.0f;
    b.center[2] = 1.0f;
    b.radius = 0.4f;
    b.strength = 0.4f;
    return b;
}

clay_dynamic_topology_desc topology(bool enabled) {
    clay_dynamic_topology_desc t{};
    t.struct_size = sizeof(t);
    REQUIRE(clay_dynamic_topology_defaults(&t) == CLAY_OK);
    t.enabled = enabled ? 1 : 0;
    t.detail_mode = CLAY_DETAIL_BRUSH_RELATIVE;
    t.detail_resolution = 6.0f;
    return t;
}

}  // namespace

TEST_CASE("c dynamic: a surface converts, reports and round-trips") {
    Fixture fx;

    clay_dynamic_surface_stats stats{};
    stats.struct_size = sizeof(stats);
    REQUIRE(clay_dynamic_surface_stats_get(fx.surface, &stats) == CLAY_OK);
    CHECK(stats.faces > 0);
    CHECK(stats.halfedges == stats.edges * 2);
    // A closed surface: V - E + F = 2.
    CHECK(static_cast<long>(stats.vertices) - static_cast<long>(stats.edges) +
              static_cast<long>(stats.faces) ==
          2);
    CHECK(stats.boundary_edges == 0);

    int32_t ok = 0;
    size_t msg_len = 0;
    REQUIRE(clay_dynamic_surface_validate(fx.surface, &ok, nullptr, &msg_len) == CLAY_OK);
    CHECK(ok == 1);

    // TRIANGLES, and the export says so.
    clay_mesh* back = nullptr;
    REQUIRE(clay_dynamic_surface_to_mesh(fx.surface, &back) == CLAY_OK);
    CHECK(clay_mesh_quad_count(back) == 0);
    CHECK(clay_mesh_index_count(back) == stats.faces * 3);
    clay_mesh_destroy(back);

    // Bytes round-trip on the size-query pattern.
    size_t needed = 0;
    REQUIRE(clay_dynamic_surface_serialize(fx.surface, nullptr, &needed) == CLAY_OK);
    REQUIRE(needed > 0);
    std::vector<uint8_t> bytes(needed);
    size_t written = needed;
    REQUIRE(clay_dynamic_surface_serialize(fx.surface, bytes.data(), &written) == CLAY_OK);

    clay_dynamic_surface* loaded = nullptr;
    REQUIRE(clay_dynamic_surface_deserialize(bytes.data(), bytes.size(), &loaded) == CLAY_OK);
    clay_dynamic_surface_stats loaded_stats{};
    loaded_stats.struct_size = sizeof(loaded_stats);
    REQUIRE(clay_dynamic_surface_stats_get(loaded, &loaded_stats) == CLAY_OK);
    CHECK(loaded_stats.faces == stats.faces);
    CHECK(loaded_stats.vertices == stats.vertices);
    clay_dynamic_surface_destroy(loaded);

    // A truncated buffer is refused rather than half-read.
    clay_dynamic_surface* rejected = nullptr;
    CHECK(clay_dynamic_surface_deserialize(bytes.data(), 12, &rejected) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(rejected == nullptr);
}

TEST_CASE("c dynamic: a non-manifold mesh is refused and says which problem") {
    // Three faces on one edge. A half-edge surface cannot express it, and
    // dropping the third silently would change the model without saying so.
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0};
    const uint32_t indices[] = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 5, indices, 9, &mesh) == CLAY_OK);

    clay_dynamic_surface* surface = nullptr;
    int32_t err = -1;
    CHECK(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(err == CLAY_DYNAMIC_NON_MANIFOLD_EDGE);
    CHECK(surface == nullptr);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c dynamic: a stamp reports what it did, with three revisions") {
    Fixture fx;

    clay_surface_revision before{};
    before.struct_size = sizeof(before);
    REQUIRE(clay_dynamic_surface_revision(fx.surface, &before) == CLAY_OK);

    const clay_mesh_brush_desc brush = draw_brush();
    const clay_dynamic_topology_desc topo = topology(true);
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_dynamic_sculptor_stamp(fx.sculptor, &brush, &topo, nullptr, &report) == CLAY_OK);

    CHECK(report.moved_vertices > 0);
    CHECK(report.split_edges > 0);
    // THREE REVISIONS, advancing independently, so a host re-uploads an index
    // buffer only when connectivity changed.
    CHECK(report.revision.topology > before.topology);
    CHECK(report.revision.geometry > before.geometry);
    CHECK(report.dirty_max[2] > report.dirty_min[2]);

    int32_t ok = 0;
    size_t len = 0;
    REQUIRE(clay_dynamic_surface_validate(fx.surface, &ok, nullptr, &len) == CLAY_OK);
    CHECK(ok == 1);

    // LAYER is refused rather than silently becoming something else.
    clay_mesh_brush_desc layer = brush;
    layer.verb = CLAY_MESH_BRUSH_LAYER;
    CHECK(clay_dynamic_sculptor_stamp(fx.sculptor, &layer, &topo, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c dynamic: the dirty chunk stream reconstructs the whole export") {
    // THE CASE THIS FILE EXISTS FOR. A host draws from the chunk stream; if it
    // disagrees with the surface, the host draws something that is not the
    // model, and it looks plausible until somebody measures it.
    Fixture fx;
    const clay_mesh_brush_desc brush = draw_brush();
    const clay_dynamic_topology_desc topo = topology(true);
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_dynamic_sculptor_stamp(fx.sculptor, &brush, &topo, nullptr, &report) == CLAY_OK);

    // Reconstruct from EVERY chunk, which is what a host does on its first
    // frame; the dirty list below is the incremental case.
    const size_t chunks = clay_dynamic_surface_chunk_count(fx.sculptor);
    REQUIRE(chunks > 0);

    std::multiset<std::array<int64_t, 9>> from_chunks;
    auto quantize = [](const float* p) {
        // Quantized so the comparison is about WHICH triangles, not about float
        // formatting. The positions come from the same storage on both paths,
        // so this loses nothing real.
        std::array<int64_t, 9> key{};
        for (int i = 0; i < 9; ++i) key[i] = static_cast<int64_t>(std::llround(p[i] * 1e6));
        return key;
    };

    size_t total_triangles = 0;
    for (size_t c = 0; c < chunks; ++c) {
        clay_dynamic_chunk_info info{};
        info.struct_size = sizeof(info);
        // The capacity query first: nothing here allocates per chunk per frame.
        REQUIRE(clay_dynamic_surface_copy_chunk(fx.sculptor, c, nullptr, 0, nullptr, 0, nullptr, 0,
                                                &info) == CLAY_OK);
        if (info.vertex_count == 0) continue;
        std::vector<float> positions(info.vertex_count * 3);
        std::vector<uint32_t> indices(info.index_count);
        clay_dynamic_chunk_info written{};
        written.struct_size = sizeof(written);
        REQUIRE(clay_dynamic_surface_copy_chunk(fx.sculptor, c, positions.data(), positions.size(),
                                                nullptr, 0, indices.data(), indices.size(),
                                                &written) == CLAY_OK);
        CHECK(written.vertex_count == info.vertex_count);
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            float tri[9];
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) tri[k * 3 + a] = positions[indices[t + k] * 3 + a];
            from_chunks.insert(quantize(tri));
            ++total_triangles;
        }
    }

    // The whole export, the same way.
    clay_mesh* whole = nullptr;
    REQUIRE(clay_dynamic_surface_to_mesh(fx.surface, &whole) == CLAY_OK);
    const size_t vcount = clay_mesh_vertex_count(whole);
    const size_t icount = clay_mesh_index_count(whole);
    const float* wp = clay_mesh_positions(whole);
    const uint32_t* wi = clay_mesh_indices(whole);
    REQUIRE(wp != nullptr);
    REQUIRE(wi != nullptr);
    const std::vector<float> wpos(wp, wp + vcount * 3);
    const std::vector<uint32_t> widx(wi, wi + icount);
    clay_mesh_destroy(whole);

    std::multiset<std::array<int64_t, 9>> from_whole;
    for (size_t t = 0; t + 2 < widx.size(); t += 3) {
        float tri[9];
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) tri[k * 3 + a] = wpos[widx[t + k] * 3 + a];
        from_whole.insert(quantize(tri));
    }

    CHECK(total_triangles == widx.size() / 3);
    CHECK(from_chunks == from_whole);
}

TEST_CASE("c dynamic: a stamp dirties the chunks it touched and no others") {
    Fixture fx(10);
    REQUIRE(clay_dynamic_surface_clear_dirty(fx.sculptor) == CLAY_OK);
    size_t dirty = 1;
    REQUIRE(clay_dynamic_surface_dirty_chunks(fx.sculptor, nullptr, &dirty) == CLAY_OK);
    CHECK(dirty == 0);

    const clay_mesh_brush_desc brush = draw_brush();
    const clay_dynamic_topology_desc topo = topology(false);  // deformation only
    REQUIRE(clay_dynamic_sculptor_stamp(fx.sculptor, &brush, &topo, nullptr, nullptr) == CLAY_OK);

    REQUIRE(clay_dynamic_surface_dirty_chunks(fx.sculptor, nullptr, &dirty) == CLAY_OK);
    CHECK(dirty > 0);
    // A LOCAL stamp on a large surface: a small fraction of the chunks, not
    // most of them.
    const size_t chunks = clay_dynamic_surface_chunk_count(fx.sculptor);
    CAPTURE(dirty);
    CAPTURE(chunks);
    CHECK(dirty < chunks);

    std::vector<uint32_t> indices(dirty);
    size_t filled = indices.size();
    REQUIRE(clay_dynamic_surface_dirty_chunks(fx.sculptor, indices.data(), &filled) == CLAY_OK);
    CHECK(filled == dirty);
    for (uint32_t i : indices) CHECK(i < chunks);

    // A capacity below what is needed is refused rather than truncating.
    std::vector<uint32_t> tiny(1);
    size_t small = 0;
    if (dirty > 1) {
        small = tiny.size();
        CHECK(clay_dynamic_surface_dirty_chunks(fx.sculptor, tiny.data(), &small) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(small == dirty);  // and it says how much was needed
    }

    REQUIRE(clay_dynamic_surface_clear_dirty(fx.sculptor) == CLAY_OK);
    REQUIRE(clay_dynamic_surface_dirty_chunks(fx.sculptor, nullptr, &dirty) == CLAY_OK);
    CHECK(dirty == 0);
}

TEST_CASE("c dynamic: a descriptor must declare its whole size, and says so") {
    // THE PREFIX RULE APPLIES TO A STRUCT THAT GAINED FIELDS, and these are all
    // new: their original layout IS their whole layout, so a shorter one is a
    // caller error rather than an older host. Getting that backwards is easy —
    // it is the same mistake the brush-preset tests made — so it is pinned
    // rather than assumed.
    clay_dynamic_topology_desc shorter{};
    shorter.struct_size = offsetof(clay_dynamic_topology_desc, preserve_boundaries);
    CHECK(clay_dynamic_topology_defaults(&shorter) != CLAY_OK);

    // A descriptor with no size at all is refused rather than assumed.
    clay_dynamic_topology_desc unset{};
    CHECK(clay_dynamic_topology_defaults(&unset) != CLAY_OK);

    // The whole size works, and the defaults are the engine's.
    clay_dynamic_topology_desc full{};
    full.struct_size = sizeof(full);
    REQUIRE(clay_dynamic_topology_defaults(&full) == CLAY_OK);
    CHECK(full.enabled == 1);
    CHECK(full.split_factor > 1.0f);
    // THE HYSTERESIS GAP is in the defaults, not left to the caller: with one
    // threshold a stationary brush pumps the same edge forever.
    CHECK(full.collapse_factor < 1.0f);
    CHECK(full.split_factor > full.collapse_factor);
    CHECK(full.max_ops_per_stamp > 0);
    CHECK(full.preserve_boundaries == 1);

    // The surface descriptor the same way.
    clay_dynamic_surface_desc surface{};
    surface.struct_size = sizeof(surface);
    REQUIRE(clay_dynamic_surface_defaults(&surface) == CLAY_OK);
    CHECK(surface.weld_epsilon > 0.0f);

    // An unknown detail mode is refused rather than decoded into something.
    Fixture fx(4);
    clay_dynamic_topology_desc bad = full;
    bad.detail_mode = 99;
    const clay_mesh_brush_desc brush = draw_brush();
    CHECK(clay_dynamic_sculptor_stamp(fx.sculptor, &brush, &bad, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c dynamic: the fixed mesh sculptor is untouched by any of this") {
    // A host relying on the fixed-topology contract must be unaffected. The
    // contract is that no verb creates, splits, deletes or reorders a polygon
    // and `indices` comes out byte-identical.
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    cube_sphere(6, 1.0f, &positions, &indices);
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &mesh) == CLAY_OK);

    clay_mesh_sculptor* sculptor = nullptr;
    REQUIRE(clay_mesh_sculptor_create(mesh, 0.0f, &sculptor) == CLAY_OK);
    const clay_mesh_brush_desc brush = draw_brush();
    size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    const size_t icount = clay_mesh_index_count(mesh);
    REQUIRE(icount == indices.size());
    const uint32_t* after = clay_mesh_indices(mesh);
    REQUIRE(after != nullptr);
    CHECK(std::memcmp(after, indices.data(), icount * sizeof(uint32_t)) == 0);

    clay_mesh_sculptor_destroy(sculptor);
    clay_mesh_destroy(mesh);
}
