#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

#include "clay.h"

// WHAT DOES THIS DOCUMENT COST? (c-abi spec: roll-up-document-memory)
//
// A rollup is easy to test vacuously — "it returned a number and the number was
// positive" passes against a stub that returns 1. So none of these assert a
// byte count. Absolute figures are not assertable at all: sizeof(Node) moves
// when a member is added, bucket_count() is implementation-defined, and
// libstdc++ and libc++ disagree on both. "An empty document is 343 bytes" would
// fail on macOS for a reason that is not a defect.
//
// Every assertion below is therefore a RATIO, a SUM, or a DIRECTION OF CHANGE
// — three properties a wrong rollup actually violates.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;

    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    clay_memory_report memory() const {
        clay_memory_report r{};
        r.struct_size = sizeof(r);
        REQUIRE(clay_document_memory(d, &r) == CLAY_OK);
        return r;
    }
    clay_memory_report layer(clay_layer_id id) const {
        clay_memory_report r{};
        r.struct_size = sizeof(r);
        REQUIRE(clay_layer_memory(d, id, &r) == CLAY_OK);
        return r;
    }
    clay_voxel_grid* add_voxels(const char* name, clay_layer_id* out) {
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d, name, 0.05f, out, &g) == CLAY_OK);
        return g;
    }
    void add_sphere(float r) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, sdf, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
};

// Every byte field except the total. Listed here rather than summed inside the
// library's own helper, so that a field added to the struct and summed only
// there still fails this test — which is the point of checking a sum from
// outside it.
std::uint64_t sum_of_parts(const clay_memory_report& r) {
    return r.edit_list + r.voxel_content + r.mesh_layers + r.masks + r.voxel_sculpt_layers +
           r.history + r.passthrough + r.transient;
}

void fill_block(clay_voxel_grid* g, std::int32_t half) {
    const std::int32_t lo[3] = {-half, -half, -half};
    const std::int32_t hi[3] = {half, half, half};
    REQUIRE(clay_voxel_fill_box(g, lo, hi, 1) == CLAY_OK);
}

std::size_t occupied(clay_voxel_grid* g) {
    std::size_t n = 0;
    REQUIRE(clay_voxel_occupied_count(g, &n) == CLAY_OK);
    return n;
}

}  // namespace

TEST_CASE("memory: an empty document reports a total rather than an error") {
    Doc doc;
    const clay_memory_report r = doc.memory();
    CHECK(r.total > 0);           // the containers themselves are not free
    CHECK(r.voxel_content == 0);  // and nothing is attributed to content
    CHECK(r.masks == 0);
    CHECK(r.mesh_layers == 0);
    CHECK(r.voxel_layers == 0);
}

TEST_CASE("memory: the report moves with the content") {
    Doc doc;
    const clay_memory_report empty = doc.memory();

    clay_layer_id vox = 0;
    clay_voxel_grid* g = doc.add_voxels("blocks", &vox);
    fill_block(g, 20);
    // NON-DEGENERATE FIRST. A rollup test on an empty grid asserts a
    // relationship between zeros and passes against anything.
    REQUIRE(occupied(g) > 10000);

    const clay_memory_report filled = doc.memory();
    CHECK(filled.voxel_content > 0);
    CHECK(filled.total > empty.total);
    // A RATIO, not a byte count: 41^3 cells must dominate an empty document by
    // a wide margin whatever a container's per-entry overhead happens to be.
    CHECK(filled.total > empty.total * 10);
    CHECK(filled.voxel_layers == 1);
}

TEST_CASE("memory: the parts account for the whole") {
    Doc doc;
    doc.add_sphere(0.5f);
    clay_layer_id vox = 0;
    clay_voxel_grid* g = doc.add_voxels("blocks", &vox);
    fill_block(g, 8);
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    doc.add_sphere(0.3f);

    const clay_memory_report r = doc.memory();
    // Summed from outside, field by field: a field added later and summed only
    // inside the library would still pass a check that restated r.total.
    CHECK(sum_of_parts(r) == r.total);
    CHECK(r.history > 0);  // undo is on and has a step, so this term is live
}

TEST_CASE("memory: a cost is attributed to the subsystem that incurred it") {
    // The assertion a total-only test cannot make, and the one that catches a
    // rollup summing into the wrong bucket or double-counting.
    Doc doc;
    clay_layer_id vox = 0;
    clay_voxel_grid* g = doc.add_voxels("blocks", &vox);
    fill_block(g, 8);
    const clay_memory_report before = doc.memory();

    clay_mask* m = nullptr;
    REQUIRE(clay_document_add_mask(doc.d, vox, 0.05f, &m) == CLAY_OK);
    const float lo[3] = {-0.4f, -0.4f, -0.4f};
    const float hi[3] = {0.4f, 0.4f, 0.4f};
    REQUIRE(clay_mask_fill(m, lo, hi, 1.0f) == CLAY_OK);
    std::size_t painted = 0;
    REQUIRE(clay_mask_painted_count(m, &painted) == CLAY_OK);
    REQUIRE(painted > 1000);  // non-degenerate: the mask really holds cells

    const clay_memory_report after = doc.memory();
    CHECK(after.masks > before.masks);
    // And nothing else moved. A mask lives beside the voxels, not inside them.
    CHECK(after.voxel_content == before.voxel_content);
    CHECK(after.edit_list == before.edit_list);
    CHECK(after.mesh_layers == before.mesh_layers);
    CHECK(after.mask_count == 1);
}

TEST_CASE("memory: a voxel layer costs CHUNKS, not cells") {
    // The property most likely to surprise a host, and the one that made the
    // first version of the test below pass vacuously: a chunk is 32^3 cells and
    // is allocated whole, so ONE voxel costs 32 KiB and a fully packed chunk
    // costs the same 32 KiB. Memory tracks the region touched, not the cells in
    // it.
    Doc doc;
    clay_layer_id sparse = 0, dense = 0;
    clay_voxel_grid* gs = doc.add_voxels("one cell", &sparse);
    clay_voxel_grid* gd = doc.add_voxels("packed", &dense);

    const std::int32_t one[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(gs, one, 1) == CLAY_OK);
    // Wholly inside the chunk the single cell above landed in, so the two grids
    // hold the same ONE chunk and differ only in how full it is.
    const std::int32_t lo[3] = {1, 1, 1};
    const std::int32_t hi[3] = {30, 30, 30};
    REQUIRE(clay_voxel_fill_box(gd, lo, hi, 1) == CLAY_OK);
    REQUIRE(occupied(gd) > occupied(gs) * 1000);  // 27 000 cells against one

    // Same cost, three orders of magnitude apart in occupancy.
    CHECK(doc.layer(dense).voxel_content == doc.layer(sparse).voxel_content);
}

TEST_CASE("memory: content sums exactly across the layers") {
    Doc doc;
    clay_layer_id a = 0, b = 0;
    clay_voxel_grid* ga = doc.add_voxels("heavy", &a);
    clay_voxel_grid* gb = doc.add_voxels("light", &b);
    // The layers must differ in CHUNK SPAN, not merely in cell count. The first
    // version of this fixture filled +/-20 against +/-3 — both straddle the
    // origin and touch exactly the same eight chunks, so the two layers
    // reported the identical figure and the asymmetry this test is about did
    // not exist. Occupancy was 200x apart and memory was equal, which is
    // correct behaviour and a useless fixture.
    fill_block(ga, 100);  // spans ~7^3 chunks
    fill_block(gb, 3);    // one octant of chunks around the origin
    REQUIRE(occupied(ga) > occupied(gb) * 10);

    const clay_memory_report doc_r = doc.memory();
    const clay_memory_report ra = doc.layer(a);
    const clay_memory_report rb = doc.layer(b);

    // The heavy layer is identifiable as the heavy one — the whole point of a
    // per-layer report.
    CHECK(ra.voxel_content > rb.voxel_content);

    // Content partitions exactly: every chunk belongs to exactly one layer id.
    CHECK(ra.voxel_content + rb.voxel_content == doc_r.voxel_content);
    CHECK(ra.masks + rb.masks == doc_r.masks);
    CHECK(ra.mesh_layers + rb.mesh_layers == doc_r.mesh_layers);
    CHECK(ra.voxel_sculpt_layers + rb.voxel_sculpt_layers == doc_r.voxel_sculpt_layers);

    // Document-wide lines are zero per layer, which is what makes the above a
    // partition rather than an accident.
    CHECK(ra.history == 0);
    CHECK(ra.passthrough == 0);

    // And the per-layer report is internally consistent too.
    CHECK(sum_of_parts(ra) == ra.total);
}

TEST_CASE("memory: an unknown layer is an error, not an empty report") {
    // A zeroed report reads as an empty layer, and a host would show that
    // confidently. The distinction has to be an error code.
    Doc doc;
    clay_memory_report r{};
    r.struct_size = sizeof(r);
    CHECK(clay_layer_memory(doc.d, 9999, &r) == CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("memory: the report is a versioned descriptor") {
    Doc doc;
    clay_memory_report r{};
    r.struct_size = 0;  // a caller that forgot
    CHECK(clay_document_memory(doc.d, &r) != CLAY_OK);
}

TEST_CASE("memory: a document that never enabled undo reports no history") {
    // Not an error: costing nothing is the honest answer, and it is the same
    // rule clay_document_history_bytes already follows.
    Doc doc;
    doc.add_sphere(0.5f);
    const clay_memory_report r = doc.memory();
    CHECK(r.history == 0);
    CHECK(sum_of_parts(r) == r.total);
}

// -- the transient figure, which the C ABI cannot show and C++ can -----------
//
// Two tests for one field, because the honest claim has two halves and
// asserting only the C++ half would leave the ABI's documented "always zero"
// as a comment nobody checks.

TEST_CASE("memory: the C ABI reports no transient memory, whatever you paint") {
    // Structural, not incidental: every mask entry point opens its step and
    // closes it before returning, and calls on one document must be serialized,
    // so there is no moment at which a caller could hold a handle, have a step
    // open, and ask. This is the assertion that fails the day an entry point
    // spanning a step is added — at which point the ABI comment saying it reads
    // zero has to change with it.
    Doc doc;
    clay_layer_id vox = 0;
    clay_voxel_grid* g = doc.add_voxels("blocks", &vox);
    fill_block(g, 8);
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);

    clay_mask* m = nullptr;
    REQUIRE(clay_document_add_mask(doc.d, vox, 0.05f, &m) == CLAY_OK);
    const float lo[3] = {-0.4f, -0.4f, -0.4f};
    const float hi[3] = {0.4f, 0.4f, 0.4f};
    REQUIRE(clay_mask_fill(m, lo, hi, 1.0f) == CLAY_OK);
    std::size_t painted = 0;
    REQUIRE(clay_mask_painted_count(m, &painted) == CLAY_OK);
    REQUIRE(painted > 1000);  // a step really did open and close in there

    const clay_memory_report r = doc.memory();
    CHECK(r.transient == 0);
    CHECK(sum_of_parts(r) == r.total);
}
