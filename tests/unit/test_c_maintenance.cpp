// The between-strokes seam across the C ABI (c-abi and sculpt-runtime specs,
// add-extreme-poly-runtime, tasks 3.3, 3.4 and 3.5).
//
// A sculpt runtime accumulates work that makes the NEXT interaction cheaper and
// this one slower, and every bit of it is a stall if it lands while a finger is
// on the glass. The C++ side has had the queue, the index measurement and the
// normal deferral since the engine stage; none of the three was reachable from
// a host, which is the same as not having shipped them — an application that
// can only call this header could queue nothing, could not ask whether its
// index had decayed, and could not defer a single normal across a drag it drove
// stamp by stamp.
//
// WHAT IS GATED HERE THAT THE C++ TESTS CANNOT REACH.
//
// THE GATE IS A MECHANISM RATHER THAN A CONVENTION, and the case that proves it
// is the one where a host asks for work MID-STROKE and gets nothing — because
// "we only call this between strokes" is a rule that survives until the second
// caller. Both halves of the drain go through the engine's `service`, so the
// case would fail if this file had read `in_stroke` and decided for itself.
//
// A REQUEST IS NOT GATED AND A DRAIN IS. That asymmetry is the whole design and
// it is easy to get backwards: a stamp is exactly where an item is DISCOVERED,
// so refusing to record one mid-stroke would lose the request rather than defer
// the work.
//
// TAKE PEEKS, COMPLETE REMOVES. A host that took an item and then found it
// could not afford it has DECLINED, and the item has to still be there. The
// case takes the same item twice.
//
// AND THE DEFERRAL'S ONE CONTRACT: the final state is EXACT either way. Two
// identical stroke sequences, one deferred and flushed, one not, compared
// normal by normal — because "deferring changes only when the work happens" is
// a claim about bytes, and a test that only asserted the flush ran would pass
// against a flush that computed something else.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clay.h"

namespace {

// A quad grid on the XZ plane as triangles, built through the C surface only.
// Manifold, closed-edge-free and free of degenerate triangles, which is what an
// adaptive surface will accept — a marching-cubes sphere is not, and refuses
// with CLAY_DYNAMIC_DEGENERATE_TRIANGLE.
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

clay_mesh* plane_mesh(int n) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    plane(n, 1.0f, &positions, &indices);
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &mesh) == CLAY_OK);
    return mesh;
}

// A MESHED SPHERE, and the reason it is not the plane above is worth stating:
// clay_mesh_from_triangles carries positions and indices and NOTHING ELSE, and
// a sculptor never manufactures normals for a mesh that has none — twelve bytes
// a vertex is a real cost to add behind a brush stroke. So a deferral is
// unobservable on a bare triangle soup, and the fixture has to be a mesh that
// came out of the library carrying normals, which is what every host actually
// sculpts.
clay_mesh* meshed_sphere(float voxel_size) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    float radius = 0.5f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(item != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    clay_mesh_params params{};
    params.struct_size = sizeof(params);
    params.voxel_size = voxel_size;
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_document_mesh(doc, &params, &mesh) == CLAY_OK);
    clay_document_destroy(doc);
    REQUIRE(clay_mesh_normals(mesh) != nullptr);
    return mesh;
}

clay_maintenance_item fresh_item() {
    clay_maintenance_item item{};
    item.struct_size = sizeof(item);
    return item;
}

// A dab centred on the +Z pole of the fixture sphere (or the plane's middle):
// `z` is the third coordinate, and y stays 0 because both fixtures straddle it.
clay_mesh_brush_desc draw_brush(float x, float z, float radius, float strength) {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.center[0] = x;
    d.center[1] = 0.0f;
    d.center[2] = z;
    d.radius = radius;
    d.strength = strength;
    return d;
}

}  // namespace

TEST_CASE("C maintenance queue: an item is a request, and requests fold") {
    clay_maintenance_queue* queue = nullptr;
    REQUIRE(clay_maintenance_queue_create(&queue) == CLAY_OK);

    size_t count = 99;
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 0);

    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_INDEX_REBUILD, 7, 0) == CLAY_OK);
    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_INDEX_REBUILD, 7, 250) ==
            CLAY_OK);
    // A DIFFERENT TARGET IS A DIFFERENT JOB. Folding on the kind alone would
    // collapse "rebuild level 7" and "rebuild level 9" into one item, and a
    // host servicing it would leave the other one silently undone.
    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_INDEX_REBUILD, 9, 0) == CLAY_OK);

    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 2);

    clay_maintenance_item item = fresh_item();
    REQUIRE(clay_maintenance_queue_item(queue, 0, &item) == CLAY_OK);
    CHECK(item.kind == CLAY_MAINTENANCE_INDEX_REBUILD);
    CHECK(item.target == 7);
    // The second request bumped the count rather than adding an entry, which is
    // what makes it safe to ask from inside a stamp.
    CHECK(item.requests == 2);
    // The LATEST non-zero estimate wins: a caller that has learned more about
    // the cost since the first request is telling the host something.
    CHECK(item.estimated_micros == 250);

    int32_t has = 0;
    REQUIRE(clay_maintenance_queue_has(queue, CLAY_MAINTENANCE_INDEX_REBUILD, 9, &has) == CLAY_OK);
    CHECK(has == 1);
    REQUIRE(clay_maintenance_queue_has(queue, CLAY_MAINTENANCE_NORMAL_FLUSH, 9, &has) == CLAY_OK);
    CHECK(has == 0);

    // Past the end is NOT_FOUND rather than a zeroed item, so a loop that ran
    // one past its count finds out.
    clay_maintenance_item past = fresh_item();
    CHECK(clay_maintenance_queue_item(queue, 2, &past) == CLAY_ERROR_NOT_FOUND);

    clay_maintenance_queue_destroy(queue);
}

TEST_CASE("C maintenance queue: a kind outside the list is refused, not clamped") {
    clay_maintenance_queue* queue = nullptr;
    REQUIRE(clay_maintenance_queue_create(&queue) == CLAY_OK);

    // THE FAILURE THIS PREVENTS IS SILENT. Mapping an unknown value onto the
    // default would queue an INDEX REBUILD for a caller that asked for
    // something else, and the host would service it without ever learning it
    // had been misheard.
    CHECK(clay_maintenance_queue_request(queue, 5, 0, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_maintenance_queue_request(queue, -1, 0, 0) == CLAY_ERROR_INVALID_ARGUMENT);

    size_t count = 99;
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 0);

    int32_t has = 1;
    CHECK(clay_maintenance_queue_has(queue, 5, 0, &has) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_maintenance_queue_complete(queue, 5, 0, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // Every declared value names itself, and one outside the list still answers
    // rather than returning null into a printf.
    CHECK(std::string(clay_maintenance_kind_text(CLAY_MAINTENANCE_NORMAL_FLUSH)) == "normal_flush");
    CHECK(std::string(clay_maintenance_kind_text(5)) == "unknown");

    clay_maintenance_queue_destroy(queue);
}

TEST_CASE("C maintenance queue: the stroke gate stops the drain and not the request") {
    clay_maintenance_queue* queue = nullptr;
    REQUIRE(clay_maintenance_queue_create(&queue) == CLAY_OK);

    REQUIRE(clay_maintenance_queue_begin_stroke(queue) == CLAY_OK);
    int32_t in_stroke = 0;
    REQUIRE(clay_maintenance_queue_in_stroke(queue, &in_stroke) == CLAY_OK);
    CHECK(in_stroke == 1);

    // A stamp is where an item is DISCOVERED. Recording it mid-stroke is the
    // whole point; what the gate stops is running it.
    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_CHUNK_COMPACTION, 3, 0) ==
            CLAY_OK);
    size_t count = 0;
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 1);

    // THE MECHANISM. A host that wired the drain to a pointer handler gets
    // nothing done rather than a stutter it will blame on the brush.
    clay_maintenance_item item = fresh_item();
    item.target = 12345;  // left alone when there is nothing to take
    int32_t have = 1;
    REQUIRE(clay_maintenance_queue_take_next(queue, &item, &have) == CLAY_OK);
    CHECK(have == 0);
    CHECK(item.target == 12345);

    int32_t completed = 1;
    REQUIRE(clay_maintenance_queue_complete(queue, CLAY_MAINTENANCE_CHUNK_COMPACTION, 3,
                                            &completed) == CLAY_OK);
    CHECK(completed == 0);
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 1);  // an item completed mid-stroke would have been RUN mid-stroke

    REQUIRE(clay_maintenance_queue_end_stroke(queue) == CLAY_OK);
    REQUIRE(clay_maintenance_queue_in_stroke(queue, &in_stroke) == CLAY_OK);
    CHECK(in_stroke == 0);

    REQUIRE(clay_maintenance_queue_take_next(queue, &item, &have) == CLAY_OK);
    CHECK(have == 1);
    CHECK(item.kind == CLAY_MAINTENANCE_CHUNK_COMPACTION);
    CHECK(item.target == 3);

    clay_maintenance_queue_destroy(queue);
}

TEST_CASE("C maintenance queue: take peeks, complete removes") {
    clay_maintenance_queue* queue = nullptr;
    REQUIRE(clay_maintenance_queue_create(&queue) == CLAY_OK);
    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_NORMAL_FLUSH, 1, 0) == CLAY_OK);
    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_DETAIL_PROMOTION, 2, 0) ==
            CLAY_OK);

    clay_maintenance_item first = fresh_item();
    clay_maintenance_item again = fresh_item();
    int32_t have = 0;
    REQUIRE(clay_maintenance_queue_take_next(queue, &first, &have) == CLAY_OK);
    REQUIRE(have == 1);
    // A HOST THAT DECLINED HAS NOT DROPPED IT. Taking twice without completing
    // returns the same item, which is what makes "I cannot afford this one
    // right now" expressible at all.
    REQUIRE(clay_maintenance_queue_take_next(queue, &again, &have) == CLAY_OK);
    CHECK(have == 1);
    CHECK(again.kind == first.kind);
    CHECK(again.target == first.target);

    size_t count = 0;
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 2);

    int32_t completed = 0;
    REQUIRE(clay_maintenance_queue_complete(queue, first.kind, first.target, &completed) ==
            CLAY_OK);
    CHECK(completed == 1);
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 1);

    // Completing one that is not there reports 0 rather than failing: a host
    // that serviced the same item twice has done no harm.
    REQUIRE(clay_maintenance_queue_complete(queue, first.kind, first.target, &completed) ==
            CLAY_OK);
    CHECK(completed == 0);

    // The queue order is the order it was asked for, so the second take is the
    // second request rather than whichever entry happened to be swapped in.
    clay_maintenance_item second = fresh_item();
    REQUIRE(clay_maintenance_queue_take_next(queue, &second, &have) == CLAY_OK);
    REQUIRE(have == 1);
    CHECK(second.kind == CLAY_MAINTENANCE_DETAIL_PROMOTION);
    CHECK(second.target == 2);

    size_t bytes = 0;
    REQUIRE(clay_maintenance_queue_bytes(queue, &bytes) == CLAY_OK);
    CHECK(bytes > 0);

    REQUIRE(clay_maintenance_queue_clear(queue) == CLAY_OK);
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == 0);
    REQUIRE(clay_maintenance_queue_take_next(queue, &second, &have) == CLAY_OK);
    CHECK(have == 0);

    clay_maintenance_queue_destroy(queue);
}

TEST_CASE("C maintenance queue: the item descriptor negotiates its size") {
    clay_maintenance_queue* queue = nullptr;
    REQUIRE(clay_maintenance_queue_create(&queue) == CLAY_OK);
    REQUIRE(clay_maintenance_queue_request(queue, CLAY_MAINTENANCE_SLOT_POOL_COMPACTION, 4, 11) ==
            CLAY_OK);

    // A struct_size below the original layout is a caller who did not set it,
    // and that has to refuse rather than read a stack value as a size.
    clay_maintenance_item stunted{};
    stunted.struct_size = 4;
    CHECK(clay_maintenance_queue_item(queue, 0, &stunted) == CLAY_ERROR_INVALID_ARGUMENT);

    int32_t have = 0;
    CHECK(clay_maintenance_queue_take_next(queue, &stunted, &have) == CLAY_ERROR_INVALID_ARGUMENT);

    // A caller compiled against the ORIGINAL layout gets exactly its own bytes
    // back and keeps the size it declared, so a field appended later cannot
    // overrun a buffer built to yesterday's header.
    clay_maintenance_item item = fresh_item();
    REQUIRE(clay_maintenance_queue_item(queue, 0, &item) == CLAY_OK);
    CHECK(item.struct_size == sizeof(clay_maintenance_item));
    CHECK(item.estimated_micros == 11);

    clay_maintenance_queue_destroy(queue);
}

TEST_CASE("C index quality: the engine measures, the host decides") {
    clay_mesh* mesh = plane_mesh(24);
    clay_dynamic_surface* surface = nullptr;
    int32_t err = 0;
    REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    clay_dynamic_sculptor* sculptor = nullptr;
    REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);

    clay_index_quality quality{};
    quality.struct_size = sizeof(quality);
    REQUIRE(clay_dynamic_sculptor_index_quality(sculptor, &quality) == CLAY_OK);
    CHECK(quality.leaf_count > 0);
    // A PROPERTY OF THE MEASURE A HOST HAS TO KNOW ABOUT, asserted here rather
    // than left for someone to discover on a flat model: `quality` is a VOLUME
    // ratio — the mean leaf box against the box around all of them — so a
    // surface with no volume reports 0 and never wants a rebuild however far
    // its partition drifts. The header says so; this is what says it is true.
    CHECK(quality.quality == 0.0f);
    CHECK(quality.wants_rebuild == 0);

    // Give it a third dimension and the number means something again. Same
    // tree, same faces: what changed is that the leaves now enclose a volume.
    const clay_mesh_brush_desc dab = draw_brush(0.0f, 0.0f, 0.5f, 0.6f);
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_dynamic_sculptor_stamp(sculptor, &dab, nullptr, nullptr, &report) == CLAY_OK);
    REQUIRE(report.moved_vertices > 0);
    REQUIRE(clay_dynamic_sculptor_index_quality(sculptor, &quality) == CLAY_OK);
    CHECK(quality.quality > 0.0f);
    CHECK(std::isfinite(quality.quality));

    clay_maintenance_queue* queue = nullptr;
    REQUIRE(clay_maintenance_queue_create(&queue) == CLAY_OK);

    // THE HOST'S HALF, ON ITS OWN. A profile that forbids rebuilds queues
    // nothing whatever the tree thinks, and this is the case that would pass by
    // accident if the two conditions had been folded into one: the tree is
    // freshly built, so it does not want a rebuild either.
    clay_sculpt_memory_profile profile{};
    profile.struct_size = sizeof(profile);
    REQUIRE(clay_sculpt_memory_profile_defaults(&profile) == CLAY_OK);
    CHECK(profile.allow_index_rebuild == 1);
    profile.allow_index_rebuild = 0;

    int32_t queued = 1;
    REQUIRE(clay_dynamic_sculptor_request_index_rebuild(sculptor, &profile, 0, queue, &queued) ==
            CLAY_OK);
    CHECK(queued == 0);

    // And the ENGINE's half: a tree that does not want a rebuild queues nothing
    // even when the host would allow it.
    profile.allow_index_rebuild = 1;
    REQUIRE(clay_dynamic_sculptor_request_index_rebuild(sculptor, &profile, 0, queue, &queued) ==
            CLAY_OK);
    CHECK(queued == (quality.wants_rebuild != 0 ? 1 : 0));
    size_t count = 0;
    REQUIRE(clay_maintenance_queue_count(queue, &count) == CLAY_OK);
    CHECK(count == static_cast<size_t>(queued));

    // A NULL profile is the default profile rather than a refusal, which is
    // what a desktop host that has never filled one passes.
    REQUIRE(clay_dynamic_sculptor_request_index_rebuild(sculptor, nullptr, 0, queue, nullptr) ==
            CLAY_OK);

    // A rebuild is what SERVICES the item; nothing above performed one.
    REQUIRE(clay_dynamic_sculptor_rebuild_index(sculptor) == CLAY_OK);
    clay_index_quality after{};
    after.struct_size = sizeof(after);
    REQUIRE(clay_dynamic_sculptor_index_quality(sculptor, &after) == CLAY_OK);
    CHECK(after.wants_rebuild == 0);

    clay_maintenance_queue_destroy(queue);
    clay_dynamic_sculptor_destroy(sculptor);
    clay_dynamic_surface_destroy(surface);
    clay_mesh_destroy(mesh);
}

TEST_CASE("C deferred normals: the final state is exact either way") {
    // TWO IDENTICAL STROKE SEQUENCES over two identical meshes, one deferring
    // and flushing at the end and one not, compared normal by normal. That is
    // the deferral's ONLY contract, and it is a claim about bytes: a test that
    // merely checked the flush ran would pass against a flush that computed
    // something else.
    const std::vector<clay_mesh_brush_desc> stroke = {
        draw_brush(-0.15f, 0.48f, 0.25f, 0.4f),
        draw_brush(0.0f, 0.50f, 0.25f, 0.4f),
        draw_brush(0.15f, 0.48f, 0.25f, 0.4f),
    };

    clay_mesh* eager_mesh = meshed_sphere(0.04f);
    clay_mesh* lazy_mesh = meshed_sphere(0.04f);
    clay_mesh_sculptor* eager = nullptr;
    clay_mesh_sculptor* lazy = nullptr;
    REQUIRE(clay_mesh_sculptor_create(eager_mesh, -1.0f, &eager) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_create(lazy_mesh, -1.0f, &lazy) == CLAY_OK);

    int32_t defer = 1;
    REQUIRE(clay_mesh_sculptor_defer_normals(lazy, &defer) == CLAY_OK);
    CHECK(defer == 0);  // off by default: a moved vertex with a stale normal shades wrong
    REQUIRE(clay_mesh_sculptor_set_defer_normals(lazy, 1) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_defer_normals(lazy, &defer) == CLAY_OK);
    CHECK(defer == 1);

    size_t moved = 0;
    for (const clay_mesh_brush_desc& dab : stroke) {
        REQUIRE(clay_mesh_sculptor_stamp(eager, &dab, nullptr, nullptr, &moved) == CLAY_OK);
        REQUIRE(moved > 0);
        REQUIRE(clay_mesh_sculptor_stamp(lazy, &dab, nullptr, nullptr, &moved) == CLAY_OK);
        REQUIRE(moved > 0);
    }

    const size_t n = clay_mesh_vertex_count(lazy_mesh);
    REQUIRE(n == clay_mesh_vertex_count(eager_mesh));
    const float* eager_positions = clay_mesh_positions(eager_mesh);
    const float* lazy_positions = clay_mesh_positions(lazy_mesh);
    const float* eager_normals = clay_mesh_normals(eager_mesh);
    const float* lazy_normals = clay_mesh_normals(lazy_mesh);
    REQUIRE(eager_normals != nullptr);
    REQUIRE(lazy_normals != nullptr);

    // THE GEOMETRY IS ALREADY IDENTICAL. Deferring touches nothing but the
    // shading, and asserting that first is what makes the normal difference
    // below attributable to the deferral rather than to a diverged stroke.
    for (size_t i = 0; i < n * 3; ++i)
        CHECK(eager_positions[i] == doctest::Approx(lazy_positions[i]));

    // And BEFORE the flush the normals differ, so the comparison after it is a
    // claim about the flush rather than about two runs that were never apart.
    size_t differing = 0;
    for (size_t i = 0; i < n * 3; ++i)
        if (std::fabs(eager_normals[i] - lazy_normals[i]) > 1e-6f) ++differing;
    CHECK(differing > 0);

    REQUIRE(clay_mesh_sculptor_flush_normals(lazy, nullptr) == CLAY_OK);
    for (size_t i = 0; i < n * 3; ++i)
        CHECK(eager_normals[i] == doctest::Approx(lazy_normals[i]).epsilon(1e-6));

    // Flushing again is a no-op rather than a second pass over stale state,
    // which is what makes "call it at the end of every stroke" safe advice.
    REQUIRE(clay_mesh_sculptor_flush_normals(lazy, nullptr) == CLAY_OK);
    for (size_t i = 0; i < n * 3; ++i)
        CHECK(eager_normals[i] == doctest::Approx(lazy_normals[i]).epsilon(1e-6));

    clay_mesh_sculptor_destroy(lazy);
    clay_mesh_sculptor_destroy(eager);
    clay_mesh_destroy(lazy_mesh);
    clay_mesh_destroy(eager_mesh);
}

TEST_CASE("C deferred normals: a deferred stroke's undo is still exact") {
    // The argument that is easy to miss. A deferred stroke's undo is exact only
    // if the FLUSH records the normals it changed into the same delta record
    // the stamps used; a host that passed NULL here would revert positions and
    // leave shading from a stroke that no longer exists.
    clay_mesh* mesh = meshed_sphere(0.04f);
    clay_mesh_sculptor* sculptor = nullptr;
    REQUIRE(clay_mesh_sculptor_create(mesh, -1.0f, &sculptor) == CLAY_OK);
    clay_mesh_deltas* deltas = clay_mesh_deltas_create();
    REQUIRE(deltas != nullptr);

    const size_t n = clay_mesh_vertex_count(mesh);
    // CHECKED, because `clay_mesh_normals` returns NULL for a mesh that carries
    // no normals and that is its documented answer, not a failure. Building a
    // vector from (nullptr, nullptr + n * 3) memmoves n * 12 bytes from address
    // zero: this test SIGSEGV'd twice on a shared machine at 13068 bytes -- the
    // 1089-vertex sphere -- and took the desktop with it, while reading as a
    // mysterious crash in the sculptor rather than as a missing check here.
    const float* base = clay_mesh_normals(mesh);
    REQUIRE(base != nullptr);
    const std::vector<float> before(base, base + n * 3);

    REQUIRE(clay_mesh_sculptor_set_defer_normals(sculptor, 1) == CLAY_OK);
    const clay_mesh_brush_desc dab = draw_brush(0.0f, 0.5f, 0.25f, 0.5f);
    size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(sculptor, &dab, nullptr, deltas, &moved) == CLAY_OK);
    REQUIRE(moved > 0);
    REQUIRE(clay_mesh_sculptor_flush_normals(sculptor, deltas) == CLAY_OK);

    REQUIRE(clay_mesh_deltas_revert(deltas, sculptor) == CLAY_OK);
    const float* after = clay_mesh_normals(mesh);
    REQUIRE(after != nullptr);
    for (size_t i = 0; i < n * 3; ++i) CHECK(after[i] == doctest::Approx(before[i]).epsilon(1e-6));

    clay_mesh_deltas_destroy(deltas);
    clay_mesh_sculptor_destroy(sculptor);
    clay_mesh_destroy(mesh);
}

TEST_CASE("C maintenance: null handles refuse rather than crash") {
    clay_maintenance_item item = fresh_item();
    int32_t flag = 0;
    size_t count = 0;
    CHECK(clay_maintenance_queue_request(nullptr, CLAY_MAINTENANCE_NORMAL_FLUSH, 0, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_maintenance_queue_count(nullptr, &count) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_maintenance_queue_item(nullptr, 0, &item) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_maintenance_queue_take_next(nullptr, &item, &flag) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_maintenance_queue_in_stroke(nullptr, &flag) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_dynamic_sculptor_index_quality(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_sculptor_flush_normals(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_multires_sculptor_flush_normals(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    // Destroying null is the same no-op every other handle's destroy is.
    clay_maintenance_queue_destroy(nullptr);
}
