// A subdivision hierarchy across the C ABI (c-abi spec, add-mesh-multires).
//
// Two things are gated here that the C++ tests cannot reach. THE CHANGED-BLOCK
// PATH IS COMPARED AGAINST THE WHOLE LEVEL — a block stream that quietly
// disagrees with the surface is a host drawing something that is not the model,
// and it would look plausible for a long time. And the BUDGET is exercised
// through the descriptor a host actually declares, because "refuses rather than
// allocating half of it" is a promise made to the host and not to the library.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

// A quad grid on the XZ plane as triangles, built through the C surface only.
void plane(int n, float half, std::vector<float>* positions, std::vector<uint32_t>* indices) {
    positions->clear();
    indices->clear();
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            positions->push_back(-half + step * static_cast<float>(x));
            positions->push_back(0.0f);
            positions->push_back(-half + step * static_cast<float>(z));
        }
    const uint32_t stride = static_cast<uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = static_cast<uint32_t>(z) * stride + static_cast<uint32_t>(x);
            const uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            indices->insert(indices->end(), {a, b, c, a, c, d});
        }
}

struct Fixture {
    clay_mesh* mesh = nullptr;
    clay_multires* surface = nullptr;
    clay_multires_sculptor* sculptor = nullptr;

    explicit Fixture(int n = 4, uint32_t levels = 2, uint64_t budget = 0) {
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        plane(n, 2.0f, &positions, &indices);
        REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                         indices.size(), &mesh) == CLAY_OK);
        clay_multires_desc desc{};
        desc.struct_size = sizeof(desc);
        REQUIRE(clay_multires_defaults(&desc) == CLAY_OK);
        desc.memory_budget = budget;
        int32_t err = -1;
        REQUIRE(clay_multires_from_mesh(mesh, &desc, &surface, &err) == CLAY_OK);
        CHECK(err == CLAY_MULTIRES_OK);
        for (uint32_t i = 0; i < levels; ++i)
            REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
        REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);
    }
    ~Fixture() {
        clay_multires_sculptor_destroy(sculptor);
        clay_multires_destroy(surface);
        clay_mesh_destroy(mesh);
    }
};

clay_mesh_brush_desc draw_brush(float radius, float strength) {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.center[0] = 0.0f;
    d.center[1] = 0.0f;
    d.center[2] = 0.0f;
    d.radius = radius;
    d.strength = strength;
    return d;
}

}  // namespace

TEST_CASE("c multires: a hierarchy is built, levelled, and reports its counts") {
    Fixture f(4, 2);
    CHECK(clay_multires_level_count(f.surface) == 3);

    uint64_t v0 = 0, fa0 = 0, v2 = 0, fa2 = 0;
    REQUIRE(clay_multires_level_counts(f.surface, 0, &v0, &fa0) == CLAY_OK);
    REQUIRE(clay_multires_level_counts(f.surface, 2, &v2, &fa2) == CLAY_OK);
    CHECK(v2 > v0);
    // Catmull-Clark over a triangle cage: three quads per triangle at level 1,
    // four per quad after that.
    CHECK(fa2 == fa0 * 3 * 4);
    CHECK(clay_multires_level_counts(f.surface, 9, &v0, &fa0) != CLAY_OK);

    // Adding a level makes it the one being worked on, which is what an artist
    // means by "subdivide".
    uint32_t sculpt = 99, display = 99;
    REQUIRE(clay_multires_sculpt_level(f.surface, &sculpt) == CLAY_OK);
    REQUIRE(clay_multires_display_level(f.surface, &display) == CLAY_OK);
    CHECK(sculpt == 2);
    CHECK(display == 2);
    // ...and the two are independent from there.
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 0) == CLAY_OK);
    REQUIRE(clay_multires_display_level(f.surface, &display) == CLAY_OK);
    CHECK(display == 2);
    CHECK(clay_multires_set_sculpt_level(f.surface, 7) != CLAY_OK);
}

TEST_CASE("c multires: a cage that cannot carry a hierarchy is refused with a reason") {
    // Three faces on one edge. The subdivision rules ask "which two faces beside
    // this edge" and there is no answer.
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0};
    const uint32_t indices[] = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 5, indices, 9, &mesh) == CLAY_OK);
    clay_multires* surface = nullptr;
    int32_t err = -1;
    CHECK(clay_multires_from_mesh(mesh, nullptr, &surface, &err) != CLAY_OK);
    CHECK(err == CLAY_MULTIRES_NON_MANIFOLD);
    CHECK(surface == nullptr);
    CHECK(std::strlen(clay_multires_error_text(err)) > 0);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c multires: adding a level reports its cost, and an over-budget one is refused whole") {
    Fixture f(8, 1);
    clay_multires_preflight p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_multires_preflight_add_level(f.surface, &p) == CLAY_OK);
    CHECK(p.level == 2);
    CHECK(p.allowed == 1);
    CHECK(p.vertices > 0);
    CHECK(p.faces > 0);
    CHECK(p.peak_bytes >= p.persistent_bytes);
    // No side effects: asking twice gives the same answer and adds nothing.
    clay_multires_preflight again{};
    again.struct_size = sizeof(again);
    REQUIRE(clay_multires_preflight_add_level(f.surface, &again) == CLAY_OK);
    CHECK(again.vertices == p.vertices);
    CHECK(clay_multires_level_count(f.surface) == 2);

    // The same cage with a budget below that cost.
    Fixture tight(8, 1, p.peak_bytes / 2);
    clay_multires_preflight refused{};
    refused.struct_size = sizeof(refused);
    REQUIRE(clay_multires_preflight_add_level(tight.surface, &refused) == CLAY_OK);
    CHECK(refused.allowed == 0);
    CHECK(refused.error == CLAY_MULTIRES_OVER_BUDGET);

    clay_multires_memory before{};
    before.struct_size = sizeof(before);
    REQUIRE(clay_multires_memory_get(tight.surface, &before) == CLAY_OK);
    int32_t err = -1;
    CHECK(clay_multires_add_level(tight.surface, nullptr, &err) != CLAY_OK);
    CHECK(err == CLAY_MULTIRES_OVER_BUDGET);
    CHECK(clay_multires_level_count(tight.surface) == 2);
    clay_multires_memory after{};
    after.struct_size = sizeof(after);
    REQUIRE(clay_multires_memory_get(tight.surface, &after) == CLAY_OK);
    // NOT HALF-BUILT: the authoritative content is exactly what it was.
    CHECK(after.authoritative == before.authoritative);
}

TEST_CASE("c multires: a cancelled level leaves nothing behind") {
    Fixture f(4, 1);
    clay_cancel_token* token = clay_cancel_token_create();
    REQUIRE(token != nullptr);
    clay_cancel_token_cancel(token);
    uint64_t sum_before = 0;
    REQUIRE(clay_multires_detail_checksum(f.surface, &sum_before) == CLAY_OK);
    int32_t err = -1;
    CHECK(clay_multires_add_level(f.surface, token, &err) == CLAY_ERROR_CANCELLED);
    CHECK(err == CLAY_MULTIRES_CANCELLED);
    CHECK(clay_multires_level_count(f.surface) == 2);
    uint64_t sum_after = 0;
    REQUIRE(clay_multires_detail_checksum(f.surface, &sum_after) == CLAY_OK);
    CHECK(sum_after == sum_before);
    clay_cancel_token_destroy(token);
}

TEST_CASE("c multires: a level exports as an ordinary mesh") {
    Fixture f(4, 2);
    clay_mesh* level = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(f.surface, 2, &level) == CLAY_OK);
    REQUIRE(level != nullptr);
    uint64_t vertices = 0, faces = 0;
    REQUIRE(clay_multires_level_counts(f.surface, 2, &vertices, &faces) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(level) == vertices);
    clay_mesh_destroy(level);

    CHECK(clay_multires_copy_level_mesh(f.surface, 9, &level) != CLAY_OK);
}

TEST_CASE("c multires: a stamp moves detail and names its level") {
    Fixture f(4, 2);
    uint64_t base0 = 0, detail0 = 0, eval0 = 0;
    REQUIRE(clay_multires_revision(f.surface, &base0, &detail0, &eval0) == CLAY_OK);

    REQUIRE(clay_multires_set_sculpt_level(f.surface, 2) == CLAY_OK);
    const clay_mesh_brush_desc brush = draw_brush(0.8f, 0.5f);
    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_multires_sculptor_stamp(f.sculptor, &brush, nullptr, &report) == CLAY_OK);
    CHECK(report.level == 2);
    CHECK(report.moved_vertices > 0);
    // A FINE stamp is detail, not cage geometry: the base revision is untouched.
    CHECK(report.detail_revision > detail0);
    CHECK(report.base_revision == base0);
    CHECK(report.evaluated_revision > eval0);

    // ...and a stamp at the cage is the other way round.
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 0) == CLAY_OK);
    REQUIRE(clay_multires_sculptor_begin_stroke(f.sculptor) == CLAY_OK);
    const clay_mesh_brush_desc coarse = draw_brush(2.0f, 0.5f);
    clay_multires_stamp_report cage{};
    cage.struct_size = sizeof(cage);
    REQUIRE(clay_multires_sculptor_stamp(f.sculptor, &coarse, nullptr, &cage) == CLAY_OK);
    CHECK(cage.level == 0);
    CHECK(cage.base_revision > report.base_revision);
    CHECK(cage.detail_revision == report.detail_revision);
}

TEST_CASE("c multires: a detail stamp drains changed blocks, not the display level") {
    Fixture f(6, 2);
    // Warm the display level and clear whatever building it reported.
    clay_mesh* whole = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(f.surface, 2, &whole) == CLAY_OK);
    const size_t display_vertices = clay_mesh_vertex_count(whole);
    clay_mesh_destroy(whole);
    REQUIRE(clay_multires_clear_dirty(f.surface) == CLAY_OK);
    CHECK(clay_multires_dirty_block_count(f.surface) == 0);

    REQUIRE(clay_multires_set_sculpt_level(f.surface, 2) == CLAY_OK);
    const clay_mesh_brush_desc brush = draw_brush(0.5f, 0.5f);
    REQUIRE(clay_multires_sculptor_stamp(f.sculptor, &brush, nullptr, nullptr) == CLAY_OK);

    size_t count = 0;
    REQUIRE(clay_multires_dirty_blocks(f.surface, nullptr, &count) == CLAY_OK);
    CHECK(count > 0);
    CHECK(count == clay_multires_dirty_block_count(f.surface));
    std::vector<uint32_t> blocks(count);
    REQUIRE(clay_multires_dirty_blocks(f.surface, blocks.data(), &count) == CLAY_OK);
    CHECK(count == blocks.size());

    // THE POINT OF THE PATH: what a host copies follows the blocks the dab
    // reached, not the size of the level it is looking at.
    size_t copied_vertices = 0;
    for (uint32_t block : blocks) {
        clay_multires_block_info info{};
        info.struct_size = sizeof(info);
        REQUIRE(clay_multires_block_info_get(f.surface, block, 2, &info) == CLAY_OK);
        CHECK(info.patch == block);
        CHECK(info.level == 2);
        CHECK(info.vertex_count > 0);
        CHECK(info.index_count % 3 == 0);

        std::vector<float> positions(info.vertex_count * 3);
        std::vector<float> normals(info.vertex_count * 3);
        std::vector<uint32_t> indices(info.index_count);
        clay_multires_block_info written{};
        written.struct_size = sizeof(written);
        REQUIRE(clay_multires_copy_block(f.surface, block, 2, positions.data(), positions.size(),
                                         normals.data(), normals.size(), indices.data(),
                                         indices.size(), &written) == CLAY_OK);
        CHECK(written.vertex_count == info.vertex_count);
        CHECK(written.index_count == info.index_count);
        for (uint32_t i : indices) CHECK(i < info.vertex_count);
        for (float v : positions) CHECK(std::isfinite(v));
        copied_vertices += info.vertex_count;

        // A capacity that cannot hold the block is refused rather than
        // half-filled: a partial block is a host drawing half this frame and
        // half the last one.
        std::vector<float> tiny(3);
        CHECK(clay_multires_copy_block(f.surface, block, 2, tiny.data(), tiny.size(), nullptr, 0,
                                       nullptr, 0, nullptr) != CLAY_OK);
    }
    CHECK(copied_vertices < display_vertices);

    REQUIRE(clay_multires_clear_dirty(f.surface) == CLAY_OK);
    CHECK(clay_multires_dirty_block_count(f.surface) == 0);
    CHECK(clay_multires_block_info_get(f.surface, 9999, 2, nullptr) != CLAY_OK);
}

TEST_CASE("c multires: the block stream agrees with the whole level") {
    // A block stream that quietly disagrees with the surface would look
    // plausible for a long time, so every block's vertices are checked against
    // the level they came from.
    Fixture f(3, 2);
    clay_mesh* whole = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(f.surface, 2, &whole) == CLAY_OK);
    const size_t n = clay_mesh_vertex_count(whole);
    const float* raw = clay_mesh_positions(whole);
    REQUIRE(raw != nullptr);
    const std::vector<float> level_positions(raw, raw + n * 3);
    clay_mesh_destroy(whole);

    uint64_t patches = 0, ignored = 0;
    REQUIRE(clay_multires_level_counts(f.surface, 0, &ignored, &patches) == CLAY_OK);
    size_t seen = 0;
    for (uint32_t p = 0; p < patches; ++p) {
        clay_multires_block_info info{};
        info.struct_size = sizeof(info);
        REQUIRE(clay_multires_block_info_get(f.surface, p, 2, &info) == CLAY_OK);
        std::vector<float> positions(info.vertex_count * 3);
        REQUIRE(clay_multires_copy_block(f.surface, p, 2, positions.data(), positions.size(),
                                         nullptr, 0, nullptr, 0, nullptr) == CLAY_OK);
        // Every block vertex is a vertex of the level, at the same place.
        for (uint32_t v = 0; v < info.vertex_count; ++v) {
            bool found = false;
            for (size_t k = 0; k < n && !found; ++k)
                found = level_positions[k * 3 + 0] == positions[v * 3 + 0] &&
                        level_positions[k * 3 + 1] == positions[v * 3 + 1] &&
                        level_positions[k * 3 + 2] == positions[v * 3 + 2];
            CHECK(found);
        }
        seen += info.vertex_count;
    }
    // The patches together cover the level, with the shared borders counted
    // twice — which is what makes a block a standalone draw.
    CHECK(seen >= n);
}

TEST_CASE("c multires: the hierarchy round-trips through its own bytes") {
    Fixture f(4, 2);
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 2) == CLAY_OK);
    const clay_mesh_brush_desc brush = draw_brush(0.8f, 0.5f);
    REQUIRE(clay_multires_sculptor_stamp(f.sculptor, &brush, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_multires_set_display_level(f.surface, 1) == CLAY_OK);
    uint64_t sum = 0;
    REQUIRE(clay_multires_detail_checksum(f.surface, &sum) == CLAY_OK);

    size_t size = 0;
    REQUIRE(clay_multires_serialize(f.surface, nullptr, &size) == CLAY_OK);
    CHECK(size > 0);
    std::vector<uint8_t> bytes(size);
    REQUIRE(clay_multires_serialize(f.surface, bytes.data(), &size) == CLAY_OK);
    CHECK(size == bytes.size());

    // A buffer that cannot hold it is refused and reports the size needed.
    size_t small = 4;
    std::vector<uint8_t> tiny(4);
    CHECK(clay_multires_serialize(f.surface, tiny.data(), &small) != CLAY_OK);
    CHECK(small == bytes.size());

    clay_multires* back = nullptr;
    REQUIRE(clay_multires_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
    CHECK(clay_multires_level_count(back) == 3);
    uint64_t back_sum = 0;
    REQUIRE(clay_multires_detail_checksum(back, &back_sum) == CLAY_OK);
    CHECK(back_sum == sum);
    uint32_t sculpt = 0, display = 0;
    REQUIRE(clay_multires_sculpt_level(back, &sculpt) == CLAY_OK);
    REQUIRE(clay_multires_display_level(back, &display) == CLAY_OK);
    CHECK(sculpt == 2);
    CHECK(display == 1);
    clay_multires_destroy(back);

    // Truncated, and with a broken magic.
    clay_multires* refused = nullptr;
    CHECK(clay_multires_deserialize(bytes.data(), 8, &refused) != CLAY_OK);
    CHECK(refused == nullptr);
    std::vector<uint8_t> wrong = bytes;
    wrong[0] ^= 0xFF;
    CHECK(clay_multires_deserialize(wrong.data(), wrong.size(), &refused) != CLAY_OK);
}

TEST_CASE("c multires: the memory report separates detail from cache") {
    Fixture f(4, 2);
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 2) == CLAY_OK);
    const clay_mesh_brush_desc brush = draw_brush(0.8f, 0.5f);
    REQUIRE(clay_multires_sculptor_stamp(f.sculptor, &brush, nullptr, nullptr) == CLAY_OK);

    clay_multires_memory m{};
    m.struct_size = sizeof(m);
    REQUIRE(clay_multires_memory_get(f.surface, &m) == CLAY_OK);
    CHECK(m.detail > 0);
    CHECK(m.rebuildable > 0);
    CHECK(m.authoritative == m.base + m.topology + m.detail);
    CHECK(m.rebuildable == m.evaluated + m.runtime_index);
    CHECK(m.total == m.authoritative + m.rebuildable);
    CHECK(m.resident_levels > 0);

    REQUIRE(clay_multires_set_display_level(f.surface, 1) == CLAY_OK);
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 1) == CLAY_OK);
    REQUIRE(clay_multires_drop_inactive_caches(f.surface) == CLAY_OK);
    clay_multires_memory after{};
    after.struct_size = sizeof(after);
    REQUIRE(clay_multires_memory_get(f.surface, &after) == CLAY_OK);
    CHECK(after.rebuildable < m.rebuildable);
    // The user's work is untouched by anything a host does under pressure.
    CHECK(after.detail == m.detail);
}

TEST_CASE("c multires: projection fits a hierarchy to a sculpt made elsewhere") {
    Fixture f(4, 2);
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(12, 2.0f, &positions, &indices);
    for (size_t v = 0; v < positions.size() / 3; ++v)
        positions[v * 3 + 1] = 0.25f * std::sin(2.0f * positions[v * 3 + 0]);
    clay_mesh* reference = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &reference) == CLAY_OK);

    clay_multires_project_desc desc{};
    desc.struct_size = sizeof(desc);
    REQUIRE(clay_multires_project_defaults(&desc) == CLAY_OK);
    desc.max_distance = 2.0f;
    clay_multires_project_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_multires_project(f.surface, reference, &desc, nullptr, &report) == CLAY_OK);
    CHECK(report.moved > 0);
    CHECK(report.max_offset > 0.0f);
    uint64_t sum = 0;
    REQUIRE(clay_multires_detail_checksum(f.surface, &sum) == CLAY_OK);

    // A cancelled projection reports and refuses.
    clay_cancel_token* token = clay_cancel_token_create();
    clay_cancel_token_cancel(token);
    CHECK(clay_multires_project(f.surface, reference, &desc, token, &report) ==
          CLAY_ERROR_CANCELLED);
    clay_cancel_token_destroy(token);
    clay_mesh_destroy(reference);
}

TEST_CASE("c multires: the descriptor rules are enforced") {
    Fixture f(3, 1);
    // A struct_size below the original layout is not a versioned descriptor.
    clay_multires_preflight too_short{};
    too_short.struct_size = 4;
    CHECK(clay_multires_preflight_add_level(f.surface, &too_short) != CLAY_OK);
    // An absurd one is a first word that is not a struct_size at all.
    clay_multires_memory absurd{};
    absurd.struct_size = 1u << 20;
    CHECK(clay_multires_memory_get(f.surface, &absurd) != CLAY_OK);

    clay_multires_desc bad{};
    bad.struct_size = sizeof(bad);
    REQUIRE(clay_multires_defaults(&bad) == CLAY_OK);
    bad.rule = 42;
    clay_multires* surface = nullptr;
    int32_t err = -1;
    CHECK(clay_multires_from_mesh(f.mesh, &bad, &surface, &err) != CLAY_OK);
    CHECK(surface == nullptr);

    // Every entry point refuses a null handle rather than dereferencing it.
    uint32_t level = 0;
    CHECK(clay_multires_sculpt_level(nullptr, &level) != CLAY_OK);
    CHECK(clay_multires_set_display_level(nullptr, 0) != CLAY_OK);
    CHECK(clay_multires_level_count(nullptr) == 0);
    CHECK(clay_multires_dirty_block_count(nullptr) == 0);
    clay_multires_sculptor_destroy(nullptr);
    clay_multires_destroy(nullptr);
}
