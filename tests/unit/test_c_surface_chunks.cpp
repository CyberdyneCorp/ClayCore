// One transport, three surfaces, across the C ABI (c-abi spec,
// add-extreme-poly-runtime).
//
// What is gated here that the C++ tests cannot reach.
//
// THE ACKNOWLEDGEMENT IS THE POINT OF THE WHOLE SEAM, and it is the one thing
// the shipped all-or-nothing `clear_dirty` structurally cannot express: a host
// that drains half a dirty set and then drops a frame must either re-upload
// everything or lose a change. So the test that matters is the one where a
// chunk changes AGAIN between the copy and the acknowledgement, and stays
// dirty — a green suite where the acknowledgement always succeeds would prove
// nothing about the case it exists for.
//
// A STALE READBACK IS IDENTIFIABLE. A host that draws a superseded chunk draws
// something the engine does not think it made, and nothing in the pixels says
// so. The revision the caller asked for comes back beside what the engine is at
// now, and that difference is the only signal there is.
//
// AND A TRIM NEVER TOUCHES THE WORK. The authoritative checksum before and
// after a CRITICAL trim is the assertion, rather than a reading of the trim's
// source: a trim that released the user's detail would still report a large and
// plausible number of bytes freed.

#include <doctest/doctest.h>

#include <algorithm>
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

void sphere(int n, float radius, std::vector<float>* positions, std::vector<uint32_t>* indices) {
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

clay_mesh* mesh_from(const std::vector<float>& positions, const std::vector<uint32_t>& indices) {
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &mesh) == CLAY_OK);
    return mesh;
}

clay_mesh_brush_desc draw_brush(float x, float y, float z, float radius, float strength) {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.center[0] = x;
    d.center[1] = y;
    d.center[2] = z;
    d.radius = radius;
    d.strength = strength;
    return d;
}

clay_chunk_readback fresh_readback() {
    clay_chunk_readback r{};
    r.struct_size = sizeof(r);
    return r;
}

// Copy one chunk into vectors the CALLER owns, sized from the capacity query.
// The whole discipline of the transport in one helper, so every case below
// exercises it rather than a shortcut around it.
clay_chunk_readback drain(clay_surface_view* view, uint32_t chunk,
                          const clay_chunk_revisions* expected, std::vector<float>* positions,
                          std::vector<uint32_t>* indices) {
    clay_chunk_readback need = fresh_readback();
    REQUIRE(clay_surface_view_copy_chunk(view, chunk, expected, nullptr, 0, nullptr, 0, nullptr,
                                         0, &need) == CLAY_OK);
    positions->assign(static_cast<std::size_t>(need.vertex_count) * 3, 0.0f);
    indices->assign(need.index_count, 0u);
    clay_chunk_readback got = fresh_readback();
    REQUIRE(clay_surface_view_copy_chunk(view, chunk, expected, positions->data(),
                                         positions->size(), nullptr, 0, indices->data(),
                                         indices->size(), &got) == CLAY_OK);
    CHECK(got.truncated == 0);
    CHECK(got.vertex_count == need.vertex_count);
    return got;
}

}  // namespace

TEST_CASE("c surface view: a fixed mesh partitions, and every triangle is in exactly one chunk") {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(24, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);

    clay_chunk_options options{};
    options.struct_size = sizeof(options);
    REQUIRE(clay_chunk_options_defaults(&options) == CLAY_OK);
    CHECK(options.min_faces <= options.target_faces);
    CHECK(options.target_faces <= options.max_faces);
    options.target_faces = 32;
    options.min_faces = 8;
    options.max_faces = 64;

    clay_surface_view* view = nullptr;
    REQUIRE(clay_surface_view_from_mesh(mesh, &options, &view) == CLAY_OK);
    CHECK(clay_surface_view_kind(view) == CLAY_SURFACE_FIXED);

    const size_t chunks = clay_surface_view_chunk_count(view);
    CHECK(chunks > 1);

    // THE BULK FILL, which is why clay_chunk_info carries no struct_size: a host
    // reads one per chunk of the surface, in one call, straight into an upload
    // loop.
    std::vector<clay_chunk_info> infos(chunks);
    REQUIRE(clay_surface_view_chunk_infos(view, nullptr, chunks, infos.data()) == CLAY_OK);

    uint32_t triangles = 0;
    for (const clay_chunk_info& info : infos) {
        CHECK(info.live == 1);
        CHECK(info.index_count % 3 == 0);
        triangles += info.index_count / 3;
    }
    // ONE CHUNK PER TRIANGLE AND NO MORE. A partition that dropped a triangle
    // and one that shared it between two chunks both draw plausibly.
    CHECK(triangles == indices.size() / 3);

    // A fixed mesh's chunks are WELDED: the chunk's own vertices, with indices
    // local to it, so it uploads as a standalone draw.
    std::vector<float> chunk_positions;
    std::vector<uint32_t> chunk_indices;
    const clay_chunk_readback got = drain(view, infos[0].chunk, nullptr, &chunk_positions,
                                          &chunk_indices);
    CHECK(got.ok == 1);
    CHECK(got.stale == 0);
    CHECK(got.vertex_count < got.index_count);  // welded: fewer vertices than corners
    for (uint32_t i : chunk_indices) CHECK(i < got.vertex_count);

    // Every copied position is a position of the source mesh, which is what
    // says the chunk-local vertex map means what it claims.
    for (uint32_t i = 0; i < got.vertex_count; ++i) {
        bool found = false;
        for (std::size_t v = 0; v < positions.size() / 3 && !found; ++v)
            found = std::abs(chunk_positions[i * 3 + 0] - positions[v * 3 + 0]) < 1e-6f &&
                    std::abs(chunk_positions[i * 3 + 1] - positions[v * 3 + 1]) < 1e-6f &&
                    std::abs(chunk_positions[i * 3 + 2] - positions[v * 3 + 2]) < 1e-6f;
        CHECK(found);
    }

    // A too-small buffer writes NOTHING, rather than a partial fill a host
    // might draw. The counts still say what it needed.
    std::vector<float> tiny(3, -1.0f);
    clay_chunk_readback refused = fresh_readback();
    CHECK(clay_surface_view_copy_chunk(view, infos[0].chunk, nullptr, tiny.data(), tiny.size(),
                                       nullptr, 0, nullptr, 0, &refused) != CLAY_OK);
    CHECK(refused.truncated == 1);
    CHECK(refused.vertex_count == got.vertex_count);
    CHECK(tiny[0] == -1.0f);

    CHECK(clay_surface_view_copy_chunk(view, 100000, nullptr, nullptr, 0, nullptr, 0, nullptr, 0,
                                       nullptr) != CLAY_OK);

    clay_surface_view_destroy(view);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c surface view: a stamp's dirty chunks drain, acknowledge, and refuse a stale ack") {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    // BIG ENOUGH THAT THE DAB IS A FRACTION OF IT, which is the property the
    // case below asserts: on a surface of seven chunks a 0.35 dab reaches all
    // seven, and "the dirty set follows the change" would pass on a transport
    // that reported everything.
    sphere(24, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);

    clay_dynamic_surface* surface = nullptr;
    int32_t err = -1;
    REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    clay_dynamic_sculptor* sculptor = nullptr;
    REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);

    clay_surface_view* view = nullptr;
    REQUIRE(clay_surface_view_from_dynamic(sculptor, &view) == CLAY_OK);
    CHECK(clay_surface_view_kind(view) == CLAY_SURFACE_ADAPTIVE);
    REQUIRE(clay_surface_view_clear_dirty(view) == CLAY_OK);

    const clay_mesh_brush_desc brush = draw_brush(0.0f, 0.0f, 1.0f, 0.15f, 0.3f);
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_dynamic_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &report) == CLAY_OK);
    CHECK(report.moved_vertices > 0);

    size_t dirty_count = 0;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &dirty_count) == CLAY_OK);
    REQUIRE(dirty_count > 0);
    // THE PREVIEW PROPERTY, at the smallest size a unit test can afford: the
    // dab reached a fraction of the surface, so the set it reports is a
    // fraction of the chunks.
    CHECK(dirty_count < clay_surface_view_chunk_count(view));
    std::vector<uint32_t> dirty(dirty_count);
    REQUIRE(clay_surface_view_dirty_chunks(view, dirty.data(), &dirty_count) == CLAY_OK);

    // A capacity below the set is refused and reports what it needed.
    size_t too_small = 0;
    CHECK(clay_surface_view_dirty_chunks(view, dirty.data(), &too_small) != CLAY_OK);
    CHECK(too_small == dirty_count);

    std::vector<float> chunk_positions;
    std::vector<uint32_t> chunk_indices;
    const clay_chunk_readback got = drain(view, dirty[0], nullptr, &chunk_positions,
                                          &chunk_indices);
    CHECK(got.ok == 1);
    // An adaptive chunk is UNWELDED: three vertices per triangle, because its
    // topology changes under the stamp being uploaded and a per-chunk vertex
    // map would have to be rebuilt per chunk per frame.
    CHECK(got.vertex_count == got.index_count);

    // The acknowledgement, against exactly what was copied: the chunk is clean.
    size_t clean = 0;
    REQUIRE(clay_surface_view_acknowledge(view, &dirty[0], &got.current, 1, &clean) == CLAY_OK);
    CHECK(clean == 1);
    size_t after = 0;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &after) == CLAY_OK);
    CHECK(after == dirty_count - 1);

    // AND THE CASE THE WHOLE MECHANISM EXISTS FOR. Stamp again over the same
    // region, then acknowledge with the revisions from BEFORE that stamp. The
    // chunk changed after the caller read it, so it stays dirty and the host
    // has not lost the change it never saw.
    REQUIRE(clay_dynamic_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &report) == CLAY_OK);
    size_t restamped = 0;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &restamped) == CLAY_OK);
    std::vector<uint32_t> again(restamped);
    REQUIRE(clay_surface_view_dirty_chunks(view, again.data(), &restamped) == CLAY_OK);

    clay_chunk_readback now = fresh_readback();
    REQUIRE(clay_surface_view_copy_chunk(view, again[0], nullptr, nullptr, 0, nullptr, 0,
                                         nullptr, 0, &now) == CLAY_OK);
    clay_chunk_revisions old = now.current;
    old.geometry = old.geometry > 0 ? old.geometry - 1 : 0;
    clean = 99;
    REQUIRE(clay_surface_view_acknowledge(view, &again[0], &old, 1, &clean) == CLAY_OK);
    CHECK(clean == 0);
    size_t still = 0;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &still) == CLAY_OK);
    CHECK(still == restamped);

    // A STALE READBACK IS IDENTIFIABLE: the revision the caller asked for comes
    // back beside the one the engine is at now.
    clay_chunk_readback stale = fresh_readback();
    REQUIRE(clay_surface_view_copy_chunk(view, again[0], &old, nullptr, 0, nullptr, 0, nullptr,
                                         0, &stale) == CLAY_OK);
    CHECK(stale.stale == 1);
    CHECK(stale.requested.geometry == old.geometry);
    CHECK(stale.current.geometry != old.geometry);

    // ...and a readback taken against what the engine IS at is not stale.
    clay_chunk_readback current = fresh_readback();
    REQUIRE(clay_surface_view_copy_chunk(view, again[0], &stale.current, nullptr, 0, nullptr, 0,
                                         nullptr, 0, &current) == CLAY_OK);
    CHECK(current.stale == 0);

    REQUIRE(clay_surface_view_clear_dirty(view) == CLAY_OK);
    size_t drained = 1;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &drained) == CLAY_OK);
    CHECK(drained == 0);

    clay_surface_view_destroy(view);
    clay_dynamic_sculptor_destroy(sculptor);
    clay_dynamic_surface_destroy(surface);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c surface view: a hierarchy level reports the same shape, per level") {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(4, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);

    clay_multires* surface = nullptr;
    int32_t err = -1;
    REQUIRE(clay_multires_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);

    clay_surface_view* view = nullptr;
    REQUIRE(clay_surface_view_from_multires(surface, 2, &view) == CLAY_OK);
    CHECK(clay_surface_view_kind(view) == CLAY_SURFACE_MULTIRES);
    CHECK(clay_surface_view_from_multires(surface, 9, &view) != CLAY_OK);

    const size_t chunks = clay_surface_view_chunk_count(view);
    REQUIRE(chunks > 0);
    std::vector<clay_chunk_info> infos(chunks);
    REQUIRE(clay_surface_view_chunk_infos(view, nullptr, chunks, infos.data()) == CLAY_OK);

    uint32_t triangles = 0;
    for (const clay_chunk_info& info : infos) triangles += info.index_count / 3;
    uint64_t vertices = 0, faces = 0;
    REQUIRE(clay_multires_level_counts(surface, 2, &vertices, &faces) == CLAY_OK);
    // Quads, triangulated into two: the chunks cover the level's faces exactly
    // once, which is the same claim the fixed case makes about its triangles.
    CHECK(triangles == faces * 2);

    std::vector<float> chunk_positions;
    std::vector<uint32_t> chunk_indices;
    const clay_chunk_readback got = drain(view, infos[0].chunk, nullptr, &chunk_positions,
                                          &chunk_indices);
    CHECK(got.ok == 1);
    for (uint32_t i : chunk_indices) CHECK(i < got.vertex_count);

    // A dab marks chunks at the level it wrote, and one acknowledgement retires
    // one of them.
    REQUIRE(clay_surface_view_clear_dirty(view) == CLAY_OK);
    clay_multires_sculptor* sculptor = nullptr;
    REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);
    const clay_mesh_brush_desc brush = draw_brush(0.0f, 0.0f, 0.0f, 0.5f, 0.2f);
    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_multires_sculptor_stamp(sculptor, &brush, nullptr, &report) == CLAY_OK);
    CHECK(report.moved_vertices > 0);

    size_t dirty_count = 0;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &dirty_count) == CLAY_OK);
    REQUIRE(dirty_count > 0);
    std::vector<uint32_t> dirty(dirty_count);
    REQUIRE(clay_surface_view_dirty_chunks(view, dirty.data(), &dirty_count) == CLAY_OK);

    clay_chunk_readback marked = fresh_readback();
    REQUIRE(clay_surface_view_copy_chunk(view, dirty[0], nullptr, nullptr, 0, nullptr, 0,
                                         nullptr, 0, &marked) == CLAY_OK);
    size_t clean = 0;
    REQUIRE(clay_surface_view_acknowledge(view, &dirty[0], &marked.current, 1, &clean) ==
            CLAY_OK);
    CHECK(clean == 1);
    size_t after = 0;
    REQUIRE(clay_surface_view_dirty_chunks(view, nullptr, &after) == CLAY_OK);
    CHECK(after == dirty_count - 1);

    clay_multires_sculptor_destroy(sculptor);
    clay_surface_view_destroy(view);
    clay_multires_destroy(surface);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c memory: the profile crosses, the ledger rolls up, and a trim keeps the work") {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(6, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);

    clay_multires* surface = nullptr;
    int32_t err = -1;
    REQUIRE(clay_multires_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);

    clay_sculpt_memory_profile profile{};
    profile.struct_size = sizeof(profile);
    REQUIRE(clay_sculpt_memory_profile_defaults(&profile) == CLAY_OK);
    // The defaults are the library as it behaved before a profile existed.
    CHECK(profile.memory_class == CLAY_MEMORY_CLASS_FULL);
    CHECK(profile.cache_budget == 0);
    CHECK(profile.allow_index_rebuild == 1);

    profile.memory_class = CLAY_MEMORY_CLASS_CONSTRAINED;
    profile.max_resident_levels = 2;
    profile.defer_normals_in_stroke = 1;
    REQUIRE(clay_multires_set_memory_profile(surface, &profile) == CLAY_OK);

    clay_sculpt_memory_profile read{};
    read.struct_size = sizeof(read);
    REQUIRE(clay_multires_memory_profile(surface, &read) == CLAY_OK);
    CHECK(read.memory_class == CLAY_MEMORY_CLASS_CONSTRAINED);
    CHECK(read.max_resident_levels == 2);
    CHECK(read.defer_normals_in_stroke == 1);

    // A value outside the declared list is a refusal, not a clamp onto the
    // default: mapping it would hide the mistake behind a plausible result.
    profile.memory_class = 77;
    CHECK(clay_multires_set_memory_profile(surface, &profile) != CLAY_OK);
    profile.memory_class = CLAY_MEMORY_CLASS_CONSTRAINED;

    // Force the caches to exist before asking what they cost.
    clay_mesh* level = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(surface, 2, &level) == CLAY_OK);
    clay_mesh_destroy(level);

    clay_memory_ledger ledger{};
    ledger.struct_size = sizeof(ledger);
    REQUIRE(clay_multires_memory_ledger(surface, &ledger) == CLAY_OK);
    CHECK(ledger.category_count == CLAY_MEMORY_CATEGORY_COUNT);
    CHECK(ledger.total == ledger.essential + ledger.rebuildable + ledger.undoable);
    CHECK(ledger.essential > 0);
    CHECK(ledger.rebuildable > 0);
    // The roll-ups are a partition of the categories, not a second count.
    uint64_t summed = 0;
    for (int i = 0; i < CLAY_MEMORY_CATEGORY_COUNT; ++i) summed += ledger.bytes[i];
    CHECK(summed == ledger.total);
    // Authoritative detail is NEVER reported as rebuildable: a host acting on
    // that distinction would delete the user's work.
    CHECK(ledger.bytes[CLAY_MEMORY_MULTIRES_DETAIL] <= ledger.essential);

    uint64_t before = 0, after = 0;
    REQUIRE(clay_multires_detail_checksum(surface, &before) == CLAY_OK);

    // THE PIN: a memory warning arriving mid-save gets an honest answer instead
    // of a document mutating under the writer.
    clay_memory_pin* pin = nullptr;
    REQUIRE(clay_memory_pin_create(&pin) == CLAY_OK);
    CHECK(clay_memory_pin_held(pin) == 0);
    REQUIRE(clay_memory_pin_acquire(pin) == CLAY_OK);
    REQUIRE(clay_memory_pin_acquire(pin) == CLAY_OK);  // reentrant
    CHECK(clay_memory_pin_held(pin) == 1);

    clay_trim_report pinned{};
    pinned.struct_size = sizeof(pinned);
    REQUIRE(clay_multires_trim(surface, CLAY_PRESSURE_CRITICAL, pin, &pinned) == CLAY_OK);
    CHECK(pinned.pinned == 1);
    CHECK(pinned.pressure == CLAY_PRESSURE_CRITICAL);
    clay_memory_ledger while_pinned{};
    while_pinned.struct_size = sizeof(while_pinned);
    REQUIRE(clay_multires_memory_ledger(surface, &while_pinned) == CLAY_OK);
    CHECK(while_pinned.rebuildable == ledger.rebuildable);

    REQUIRE(clay_memory_pin_release(pin) == CLAY_OK);
    CHECK(clay_memory_pin_held(pin) == 1);  // the outer scope still holds it
    REQUIRE(clay_memory_pin_release(pin) == CLAY_OK);
    CHECK(clay_memory_pin_held(pin) == 0);
    // An unbalanced release leaves the count at zero rather than underflowing
    // to "pinned forever", which no trim could ever help.
    REQUIRE(clay_memory_pin_release(pin) == CLAY_OK);
    CHECK(clay_memory_pin_held(pin) == 0);

    clay_trim_report trimmed{};
    trimmed.struct_size = sizeof(trimmed);
    REQUIRE(clay_multires_trim(surface, CLAY_PRESSURE_CRITICAL, pin, &trimmed) == CLAY_OK);
    CHECK(trimmed.pinned == 0);
    CHECK(trimmed.total_released > 0);
    uint64_t released = 0;
    for (int i = 0; i < CLAY_MEMORY_CATEGORY_COUNT; ++i) released += trimmed.released[i];
    CHECK(released == trimmed.total_released);
    // Nothing essential went, at the hardest pressure there is.
    CHECK(trimmed.released[CLAY_MEMORY_MULTIRES_DETAIL] == 0);
    CHECK(trimmed.released[CLAY_MEMORY_BASE_GEOMETRY] == 0);
    CHECK(trimmed.released[CLAY_MEMORY_TOPOLOGY] == 0);

    // THE ASSERTION THE WHOLE TRIM RESTS ON. A trim that released the user's
    // work would still report a large and plausible number of bytes freed; the
    // checksum is what says it did not.
    REQUIRE(clay_multires_detail_checksum(surface, &after) == CLAY_OK);
    CHECK(after == before);

    // ...and the dropped caches reconstruct, so the level is still readable.
    clay_mesh* rebuilt = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(surface, 2, &rebuilt) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(rebuilt) > 0);
    clay_mesh_destroy(rebuilt);

    CHECK(clay_multires_trim(surface, 42, nullptr, &trimmed) != CLAY_OK);

    clay_memory_pin_destroy(pin);
    clay_multires_destroy(surface);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c memory: a document report folds in the surfaces the host holds beside it") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);

    clay_memory_report bare{};
    bare.struct_size = sizeof(bare);
    REQUIRE(clay_document_memory(doc, &bare) == CLAY_OK);
    // A document that holds no surface reports zero for them rather than
    // guessing: a hierarchy is OPAQUE and OWNING and lives beside a document.
    CHECK(bare.surface_content == 0);
    CHECK(bare.multires_detail == 0);
    CHECK(bare.total == bare.essential + bare.rebuildable + bare.undoable);

    clay_memory_ledger surfaces{};
    surfaces.struct_size = sizeof(surfaces);
    surfaces.bytes[CLAY_MEMORY_BASE_GEOMETRY] = 1024;
    surfaces.bytes[CLAY_MEMORY_MULTIRES_DETAIL] = 2048;
    surfaces.bytes[CLAY_MEMORY_EVALUATED_CACHE] = 4096;

    clay_memory_report with{};
    with.struct_size = sizeof(with);
    REQUIRE(clay_document_memory_with_surfaces(doc, &surfaces, &with) == CLAY_OK);
    CHECK(with.surface_content == 1024);
    CHECK(with.multires_detail == 2048);
    CHECK(with.surface_caches == 4096);
    CHECK(with.total == bare.total + 1024 + 2048 + 4096);
    CHECK(with.total == with.essential + with.rebuildable + with.undoable);

    // A NULL ledger is exactly clay_document_memory, which is why that call
    // stays rather than gaining an argument.
    clay_memory_report none{};
    none.struct_size = sizeof(none);
    REQUIRE(clay_document_memory_with_surfaces(doc, nullptr, &none) == CLAY_OK);
    CHECK(none.total == bare.total);

    clay_document_destroy(doc);
}

TEST_CASE("c preflight: an operation is priced before it is paid, and refuses whole") {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    sphere(6, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);

    clay_surface_preflight p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_mesh_preflight_to_dynamic(mesh, 0, &p) == CLAY_OK);
    CHECK(p.allowed == 1);
    CHECK(p.error == CLAY_BUDGET_OK);
    CHECK(p.persistent_bytes == p.authoritative_bytes + p.runtime_bytes);
    // THE PEAK IS THE NUMBER THAT MATTERS: the conversion holds the source
    // mesh, the half-edge structure and the weld map at once, and an operation
    // priced by its result terminates the process half way through.
    CHECK(p.peak_bytes >= p.persistent_bytes);

    clay_surface_preflight refused{};
    refused.struct_size = sizeof(refused);
    REQUIRE(clay_mesh_preflight_to_dynamic(mesh, 1024, &refused) == CLAY_OK);
    CHECK(refused.allowed == 0);
    CHECK(refused.error == CLAY_BUDGET_OVER_BUDGET);
    // A refusal still reports the figures, so a host can say how far over it is.
    CHECK(refused.peak_bytes == p.peak_bytes);

    // An estimate that overflows 64 bits refuses at ANY budget, including no
    // budget: a wrapped estimate is a SMALL one, and a small one is allowed.
    clay_surface_preflight overflowed{};
    overflowed.struct_size = sizeof(overflowed);
    REQUIRE(clay_mesh_preflight_global_remesh(mesh, UINT64_MAX, 0, &overflowed) == CLAY_OK);
    CHECK(overflowed.allowed == 0);
    CHECK(overflowed.error == CLAY_BUDGET_OVERFLOW);
    CHECK(std::strcmp(clay_budget_error_text(overflowed.error), "") != 0);

    clay_dynamic_surface* surface = nullptr;
    int32_t err = -1;
    REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    clay_surface_preflight back{};
    back.struct_size = sizeof(back);
    REQUIRE(clay_dynamic_surface_preflight_to_mesh(surface, 0, &back) == CLAY_OK);
    CHECK(back.allowed == 1);
    clay_surface_preflight blob{};
    blob.struct_size = sizeof(blob);
    REQUIRE(clay_dynamic_surface_preflight_encode(surface, 0, &blob) == CLAY_OK);
    CHECK(blob.peak_bytes > blob.persistent_bytes);

    // The struct_size rule, in both directions: a size below the original
    // layout is not a versioned descriptor.
    clay_surface_preflight short_desc{};
    short_desc.struct_size = 4;
    CHECK(clay_mesh_preflight_to_dynamic(mesh, 0, &short_desc) != CLAY_OK);

    clay_dynamic_surface_destroy(surface);
    clay_mesh_destroy(mesh);
}

// -- 6.5: the dirty stream against the whole-surface path --------------------------
//
// The whole-surface path stays in the library for correctness and the
// incremental path is what a host at twenty million vertices uses. That
// arrangement is only worth having if the two agree, and nothing in the pixels
// says when they stop: a host drawing from the dirty stream draws something the
// engine does not think it made, and it looks like geometry.
//
// COMPARED AS TRIANGLES, NOT AS INDEX BUFFERS, because the two paths number
// their vertices differently on purpose — the whole-surface path emits the
// level's own global ids and a chunk emits ids local to itself, which is what
// lets a host upload a chunk as a standalone draw. Canonicalised by ROTATING
// each triangle so its smallest corner is first: a rotation and not a sort,
// which would lose the winding and call an inside-out surface equal.

namespace {

struct CanonicalTri {
    uint32_t bits[9] = {};
    bool operator<(const CanonicalTri& o) const {
        return std::memcmp(bits, o.bits, sizeof(bits)) < 0;
    }
    bool operator==(const CanonicalTri& o) const {
        return std::memcmp(bits, o.bits, sizeof(bits)) == 0;
    }
};

CanonicalTri canonical(const float* a, const float* b, const float* c) {
    const float* corner[3] = {a, b, c};
    uint32_t key[3][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) std::memcpy(&key[i][k], corner[i] + k, sizeof(uint32_t));
    int first = 0;
    for (int i = 1; i < 3; ++i)
        if (std::memcmp(key[i], key[first], sizeof(key[0])) < 0) first = i;
    CanonicalTri t;
    for (int k = 0; k < 3; ++k)
        std::memcpy(t.bits + k * 3, key[(first + k) % 3], sizeof(key[0]));
    return t;
}

// Every triangle of a mesh, through the C ABI's own readback.
std::vector<CanonicalTri> triangles_of(const clay_mesh* mesh) {
    const size_t vertices = clay_mesh_vertex_count(mesh);
    const size_t index_count = clay_mesh_index_count(mesh);
    std::vector<uint32_t> indices(index_count);
    REQUIRE(clay_mesh_copy_indices(mesh, indices.data(), indices.size()) == CLAY_OK);
    const float* positions = clay_mesh_positions(mesh);
    REQUIRE(positions != nullptr);
    std::vector<CanonicalTri> out;
    out.reserve(index_count / 3);
    for (size_t i = 0; i + 2 < index_count; i += 3) {
        REQUIRE(indices[i] < vertices);
        out.push_back(canonical(positions + indices[i + 0] * 3, positions + indices[i + 1] * 3,
                                positions + indices[i + 2] * 3));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("c surface view: the chunk stream reassembles into the whole-surface path") {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(6, 1.0f, &positions, &indices);
    clay_mesh* cage = mesh_from(positions, indices);
    clay_multires* surface = nullptr;
    REQUIRE(clay_multires_from_mesh(cage, nullptr, &surface, nullptr) == CLAY_OK);
    clay_mesh_destroy(cage);
    REQUIRE(clay_multires_add_level(surface, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, nullptr) == CLAY_OK);

    uint32_t level = 0;
    REQUIRE(clay_multires_sculpt_level(surface, &level) == CLAY_OK);

    clay_surface_view* view = nullptr;
    REQUIRE(clay_surface_view_from_multires(surface, level, &view) == CLAY_OK);
    const size_t chunks = clay_surface_view_chunk_count(view);
    REQUIRE(chunks > 1);

    // A HOST'S OWN COPY, assembled chunk by chunk, each with local indices and
    // none of them knowing about the others.
    std::vector<CanonicalTri> streamed;
    for (uint32_t i = 0; i < static_cast<uint32_t>(chunks); ++i) {
        std::vector<float> chunk_positions;
        std::vector<uint32_t> chunk_indices;
        const clay_chunk_readback got = drain(view, i, nullptr, &chunk_positions, &chunk_indices);
        REQUIRE(got.ok);
        for (size_t k = 0; k + 2 < chunk_indices.size(); k += 3) {
            REQUIRE(chunk_indices[k] < got.vertex_count);
            streamed.push_back(canonical(chunk_positions.data() + chunk_indices[k + 0] * 3,
                                         chunk_positions.data() + chunk_indices[k + 1] * 3,
                                         chunk_positions.data() + chunk_indices[k + 2] * 3));
        }
    }
    std::sort(streamed.begin(), streamed.end());

    // THE WHOLE-SURFACE PATH, which the library still ships and which this
    // exists to stay equal to.
    clay_mesh* whole = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(surface, level, &whole) == CLAY_OK);
    const std::vector<CanonicalTri> reference = triangles_of(whole);
    clay_mesh_destroy(whole);

    REQUIRE(streamed.size() == reference.size());
    CHECK(streamed == reference);

    clay_surface_view_destroy(view);
    clay_multires_destroy(surface);
}

TEST_CASE("c regression: a trim between two stamps does not eat the next dab") {
    // THE HOST-FACING HALF of the defect in `test_extreme_poly_exactness.cpp`,
    // and the path it would actually have shipped on: a host answering an
    // operating-system memory warning calls clay_multires_trim, and the next
    // clay_multires_sculptor_stamp returns CLAY_OK with a plausible
    // moved_vertices while writing nothing at all.
    //
    // `moved_vertices` is exactly what a host would have checked, and it was
    // never wrong — the sculptor did move the classes, into storage the trim had
    // already released. So the assertion here is the AUTHORITATIVE CHECKSUM,
    // which is what "the dab landed" means for a hierarchy, taken after every
    // dab rather than once at the end: with the defect present the checksum
    // stands still on every second dab.
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(6, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);
    clay_multires* surface = nullptr;
    REQUIRE(clay_multires_from_mesh(mesh, nullptr, &surface, nullptr) == CLAY_OK);
    clay_mesh_destroy(mesh);
    REQUIRE(clay_multires_add_level(surface, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_multires_add_level(surface, nullptr, nullptr) == CLAY_OK);

    clay_multires_sculptor* sculptor = nullptr;
    REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);

    uint64_t previous = 0;
    REQUIRE(clay_multires_detail_checksum(surface, &previous) == CLAY_OK);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const clay_mesh_brush_desc brush =
            draw_brush(-0.3f + 0.2f * static_cast<float>(i), 0.0f, 0.0f, 0.35f, 0.5f);
        clay_multires_stamp_report report{};
        report.struct_size = sizeof(report);
        REQUIRE(clay_multires_sculptor_stamp(sculptor, &brush, nullptr, &report) == CLAY_OK);
        REQUIRE(report.moved_vertices > 0);

        uint64_t now = 0;
        REQUIRE(clay_multires_detail_checksum(surface, &now) == CLAY_OK);
        CHECK(now != previous);
        previous = now;

        clay_trim_report trimmed{};
        trimmed.struct_size = sizeof(trimmed);
        REQUIRE(clay_multires_trim(surface, CLAY_PRESSURE_CRITICAL, nullptr, &trimmed) ==
                CLAY_OK);
        // And the release itself changed nothing, which is the older claim this
        // one sits beside rather than replaces.
        uint64_t after_trim = 0;
        REQUIRE(clay_multires_detail_checksum(surface, &after_trim) == CLAY_OK);
        CHECK(after_trim == previous);
    }

    clay_multires_sculptor_destroy(sculptor);
    clay_multires_destroy(surface);
}

TEST_CASE("c surface view: an absurd declared size is refused as a size, not read as a layout") {
    // The descriptor rule, on the descriptors this change ADDED. A struct_size
    // is the one field a caller can get wrong in a way the library cannot
    // detect from the bytes: 0 is what a caller who never set it passes, a
    // small number is a struct from before the convention whose first word is a
    // float or an enum, and a huge one is a wild pointer's contents. All three
    // have to come back as a refusal rather than as a read of whatever is
    // there, and NOTHING may be written to the caller's buffer on the way out.
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(4, 1.0f, &positions, &indices);
    clay_mesh* mesh = mesh_from(positions, indices);
    clay_surface_view* view = nullptr;
    REQUIRE(clay_surface_view_from_mesh(mesh, nullptr, &view) == CLAY_OK);

    const uint32_t absurd[] = {0u, 4u, 0xffffffffu};
    for (uint32_t declared : absurd) {
        CAPTURE(declared);

        clay_chunk_options options{};
        options.struct_size = declared;
        options.target_faces = 0xdeadbeefu;
        CHECK(clay_chunk_options_defaults(&options) != CLAY_OK);
        CHECK(options.target_faces == 0xdeadbeefu);

        clay_sculpt_memory_profile profile{};
        profile.struct_size = declared;
        profile.cache_budget = 12345u;
        CHECK(clay_sculpt_memory_profile_defaults(&profile) != CLAY_OK);
        CHECK(profile.cache_budget == 12345u);

        // An INPUT descriptor with the same broken size, which is the more
        // dangerous direction: it is read rather than written.
        clay_chunk_options as_input{};
        as_input.struct_size = declared;
        as_input.target_faces = 128;
        as_input.min_faces = 32;
        as_input.max_faces = 256;
        clay_surface_view* refused = nullptr;
        CHECK(clay_surface_view_from_mesh(mesh, &as_input, &refused) != CLAY_OK);
        CHECK(refused == nullptr);

        clay_chunk_readback readback{};
        readback.struct_size = declared;
        readback.vertex_count = 999u;
        CHECK(clay_surface_view_copy_chunk(view, 0, nullptr, nullptr, 0, nullptr, 0, nullptr, 0,
                                           &readback) != CLAY_OK);
        CHECK(readback.vertex_count == 999u);
    }

    // A CHUNK ID THAT NAMES NOTHING is answered rather than read. A host keeps
    // ids across frames and a partitioner retires chunks, so a stale id in a
    // list is ordinary rather than exceptional — `live` is the answer, and the
    // bulk fill must not walk off its table looking for one.
    const uint32_t ids[] = {0u, 0xfffffffeu};
    clay_chunk_info infos[2]{};
    infos[1].live = 1;
    REQUIRE(clay_surface_view_chunk_infos(view, ids, 2, infos) == CLAY_OK);
    CHECK(infos[0].live == 1);
    CHECK(infos[1].live == 0);
    CHECK(infos[1].chunk == 0xfffffffeu);
    CHECK(infos[1].vertex_count == 0);

    // And the ordered form refuses to be asked for more chunks than exist,
    // rather than filling the tail with zeroed records a host would draw.
    const size_t chunks = clay_surface_view_chunk_count(view);
    std::vector<clay_chunk_info> too_many(chunks + 4);
    CHECK(clay_surface_view_chunk_infos(view, nullptr, chunks + 4, too_many.data()) != CLAY_OK);

    clay_surface_view_destroy(view);
    clay_mesh_destroy(mesh);
}
