#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay.h"

// Undo across the ABI, widened (c-abi spec: undo across the ABI).
//
// clay_document_undo used to act on the command stack alone. A host that
// sculpted a voxel layer and pressed undo reversed an unrelated SDF edit, or
// was told there was nothing to undo — which is the defect this file is the
// regression for. The entry points keep their signatures and reverse MORE than
// they did, rather than differently.

namespace {

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;
    clay_layer_id voxels = 0;

    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &voxels, &g) == CLAY_OK);
        REQUIRE(clay_document_enable_undo(d) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    clay_voxel_grid* grid() {
        clay_layer_id id = 0;
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_voxel_layer(d, "blocks", &id, &g) == CLAY_OK);
        return g;
    }
    std::size_t depth() const {
        std::size_t undo = 0;
        REQUIRE(clay_document_undo_state(d, nullptr, &undo, nullptr) == CLAY_OK);
        return undo;
    }
    std::size_t cells() {
        std::size_t n = 0;
        REQUIRE(clay_voxel_occupied_count(grid(), &n) == CLAY_OK);
        return n;
    }
    void add_sphere(float r) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, sdf, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
    bool undo() {
        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(d, &undone) == CLAY_OK);
        return undone != 0;
    }
    bool redo() {
        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(d, &redone) == CLAY_OK);
        return redone != 0;
    }
};

}  // namespace

TEST_CASE("c abi: undo reverses a voxel edit") {
    // The regression. Before this change the call reversed an unrelated SDF
    // edit or reported nothing to undo.
    Doc doc;
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), cell, 1) == CLAY_OK);
    REQUIRE(doc.cells() == 1);
    CHECK(doc.depth() == 1);

    CHECK(doc.undo());
    CHECK(doc.cells() == 0);
    CHECK(doc.depth() == 0);

    CHECK(doc.redo());
    CHECK(doc.cells() == 1);
}

TEST_CASE("c abi: one undo order spans the edit list and a voxel layer") {
    Doc doc;
    doc.add_sphere(0.5f);
    const std::int32_t a[3] = {0, 0, 0};
    const std::int32_t b[3] = {1, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), a, 1) == CLAY_OK);
    REQUIRE(clay_voxel_set(doc.grid(), b, 1) == CLAY_OK);
    CHECK(doc.depth() == 3);
    CHECK(doc.cells() == 2);

    // Newest first: the two voxel writes, then the SDF item.
    CHECK(doc.undo());
    CHECK(doc.cells() == 1);
    CHECK(doc.undo());
    CHECK(doc.cells() == 0);

    size_t nodes = 0;
    REQUIRE(clay_layer_node_count(doc.d, doc.sdf, &nodes) == CLAY_OK);
    CHECK(nodes == 1);  // still there — the voxel undos did not touch it
    CHECK(doc.undo());
    REQUIRE(clay_layer_node_count(doc.d, doc.sdf, &nodes) == CLAY_OK);
    CHECK(nodes == 0);

    CHECK_FALSE(doc.undo());
    CHECK(doc.depth() == 0);
}

TEST_CASE("c abi: a sculpt verb is one undo step, not one per cell") {
    Doc doc;
    // Lay down a block so a verb has something to work on.
    const std::int32_t lo[3] = {0, 0, 0};
    const std::int32_t hi[3] = {4, 4, 4};
    REQUIRE(clay_voxel_fill_box(doc.grid(), lo, hi, 1) == CLAY_OK);
    const std::size_t filled = doc.cells();
    REQUIRE(filled > 1);
    CHECK(doc.depth() == 1);  // one fill, one step, however many cells

    clay_brush_params brush{};
    brush.struct_size = sizeof(brush);
    brush.size = 3;
    brush.shape = 1;      /* sphere */
    brush.falloff = 0;
    brush.strength = 1.0f;
    const std::int32_t at[3] = {0, 0, 0};  // a corner: smoothing must remove cells
    brush.size = 5;
    REQUIRE(clay_voxel_sculpt_smooth(doc.grid(), at, &brush) == CLAY_OK);

    const std::size_t after = doc.cells();
    // The verb must have DONE something, or this measures the fill's undo
    // instead — which is what it did when first written.
    REQUIRE(after != filled);
    CHECK(doc.depth() == 2);  // one fill, one smooth: two steps for many cells

    CHECK(doc.undo());
    CHECK(doc.cells() == filled);  // the whole verb came back in one step

    CHECK(doc.redo());
    CHECK(doc.cells() == after);
}

TEST_CASE("c abi: a voxel edit that changed nothing is not an undo step") {
    Doc doc;
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), cell, 1) == CLAY_OK);
    const std::size_t before = doc.depth();

    // Erasing an already-empty cell writes nothing.
    const std::int32_t empty[3] = {50, 50, 50};
    REQUIRE(clay_voxel_erase(doc.grid(), empty) == CLAY_OK);
    CHECK(doc.depth() == before);
}

TEST_CASE("c abi: a standalone grid has no history and still edits") {
    // Undo is a DOCUMENT concept. A grid created outside one is not in a
    // document, so it records nothing — and must still work.
    clay_voxel_grid* g = clay_voxel_grid_create(0.1f);
    REQUIRE(g != nullptr);
    const std::int32_t cell[3] = {0, 0, 0};
    CHECK(clay_voxel_set(g, cell, 1) == CLAY_OK);
    std::size_t n = 0;
    CHECK(clay_voxel_occupied_count(g, &n) == CLAY_OK);
    CHECK(n == 1);
    clay_voxel_grid_destroy(g);
}

TEST_CASE("c abi: undo disabled leaves voxel editing exactly as it was") {
    clay_document* d = clay_document_create();
    clay_layer_id voxels = 0;
    clay_voxel_grid* g = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &voxels, &g) == CLAY_OK);
    // No clay_document_enable_undo.
    const std::int32_t cell[3] = {0, 0, 0};
    CHECK(clay_voxel_set(g, cell, 1) == CLAY_OK);
    std::size_t n = 0;
    CHECK(clay_voxel_occupied_count(g, &n) == CLAY_OK);
    CHECK(n == 1);

    std::int32_t enabled = 1;
    CHECK(clay_document_undo_state(d, &enabled, nullptr, nullptr) == CLAY_OK);
    CHECK(enabled == 0);
    clay_document_destroy(d);
}

TEST_CASE("c abi: consolidate is undoable, which is worth pinning") {
    // It is the operation most often assumed not to be — it takes an UndoStack
    // and records through the command vocabulary. The session is told
    // afterwards how many entries appeared, and this is what checks that the
    // telling happened.
    Doc doc;
    doc.add_sphere(0.5f);
    const std::size_t before = doc.depth();

    clay_consolidation_params params{};
    params.struct_size = sizeof(params);
    params.cell_size = 0.05f;
    if (clay_layer_consolidate(doc.d, doc.sdf, &params, nullptr, nullptr, nullptr) != CLAY_OK)
        return;

    CHECK(doc.depth() > before);  // it became a step
    CHECK(doc.undo());
}

// -- a crossing, and the layer it creates (#341) ------------------------------
//
// `unify-the-undo-history` made the FILLING of a layer undoable and left the
// layer's own creation unrecorded, so a conversion — make a voxel layer,
// rasterize a starting form into it — recorded exactly half of itself. One
// undo emptied the new layer and left it standing, which is not a state any
// user asked for and not one a host could repair: clay_document_remove_layer
// records too, so the obvious repair was itself an undo step.
//
// These build their own documents rather than using `Doc`, because the fixture
// creates its layers BEFORE enabling undo and the creation is the subject here.

namespace {

// A document with an SDF sphere to convert, undo enabled and nothing recorded
// yet. Returns the layer holding the sphere.
struct Crossing {
    clay_document* d = nullptr;
    clay_layer_id sdf = 0;

    Crossing() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &sdf) == CLAY_OK);
        float r = 0.5f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        clay_node_id node = 0;
        REQUIRE(clay_layer_add_item(d, sdf, it, &node) == CLAY_OK);
        clay_item_destroy(it);
        // AFTER the sphere, so the starting depth is zero and every step
        // counted below is one this test made.
        REQUIRE(clay_document_enable_undo(d) == CLAY_OK);
    }
    ~Crossing() { clay_document_destroy(d); }
    Crossing(const Crossing&) = delete;
    Crossing& operator=(const Crossing&) = delete;

    std::size_t depth() const {
        std::size_t undo = 0;
        REQUIRE(clay_document_undo_state(d, nullptr, &undo, nullptr) == CLAY_OK);
        return undo;
    }
    std::size_t layers() const {
        std::size_t n = 0;
        REQUIRE(clay_document_layer_count(d, &n) == CLAY_OK);
        return n;
    }
    bool undo() {
        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(d, &undone) == CLAY_OK);
        return undone != 0;
    }
    bool redo() {
        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(d, &redone) == CLAY_OK);
        return redone != 0;
    }
    // Make a voxel layer and rasterize the sphere into it. The region really
    // contains the sphere, so cells actually land — a crossing that filled
    // nothing would pass every assertion below while measuring nothing.
    clay_voxel_grid* fill(clay_layer_id* out_layer) {
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.05f, out_layer, &g) == CLAY_OK);
        const float lo[3] = {-1.0f, -1.0f, -1.0f};
        const float hi[3] = {1.0f, 1.0f, 1.0f};
        REQUIRE(clay_voxel_rasterize(g, d, lo, hi) == CLAY_OK);
        return g;
    }
};

std::size_t occupied(clay_voxel_grid* g) {
    std::size_t n = 0;
    REQUIRE(clay_voxel_occupied_count(g, &n) == CLAY_OK);
    return n;
}

}  // namespace

TEST_CASE("c abi: creating a voxel layer is an undo step") {
    // The narrow half of #341. clay_add_sdf_layer and
    // clay_document_add_mesh_layer both went through the command vocabulary;
    // clay_document_add_voxel_layer mutated the document directly, so this
    // depth was 0 and the undo below reversed something older.
    clay_document* d = clay_document_create();
    REQUIRE(d != nullptr);
    REQUIRE(clay_document_enable_undo(d) == CLAY_OK);

    clay_layer_id id = 0;
    clay_voxel_grid* g = nullptr;
    REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &id, &g) == CLAY_OK);

    std::size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(d, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 1);
    std::size_t layers = 0;
    REQUIRE(clay_document_layer_count(d, &layers) == CLAY_OK);
    CHECK(layers == 1);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(d, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_layer_count(d, &layers) == CLAY_OK);
    CHECK(layers == 0);
    // Gone from the document, not merely hidden.
    clay_layer_id found = 0;
    clay_voxel_grid* back = nullptr;
    CHECK(clay_document_voxel_layer(d, "blocks", &found, &back) == CLAY_ERROR_NOT_FOUND);

    std::int32_t redone = 0;
    REQUIRE(clay_document_redo(d, &redone) == CLAY_OK);
    CHECK(redone == 1);
    REQUIRE(clay_document_voxel_layer(d, "blocks", &found, &back) == CLAY_OK);
    CHECK(found == id);  // the same layer, not a new one wearing its name

    clay_document_destroy(d);
}

TEST_CASE("c abi: a bracketed crossing is one undo step, layer and fill together") {
    // The whole of #341, and the case the issue was filed on. Both halves have
    // to hold at once: the creation must record (or the fill is undone alone,
    // leaving an empty layer standing) AND the bracket must span step kinds (or
    // it records two steps and the first undo removes the layer out from under
    // the fill it contains).
    Crossing c;
    const std::size_t before = c.depth();
    CHECK(before == 0);

    REQUIRE(clay_document_begin_undo_group(c.d) == CLAY_OK);
    clay_layer_id vox = 0;
    clay_voxel_grid* g = c.fill(&vox);
    REQUIRE(clay_document_end_undo_group(c.d) == CLAY_OK);

    const std::size_t filled = occupied(g);
    CHECK(filled > 0);  // the crossing really converted something
    CHECK(c.layers() == 2);
    CHECK(c.depth() == before + 1);  // ONE step, not two

    CHECK(c.undo());
    CHECK(c.layers() == 1);  // the layer went with the fill
    clay_layer_id found = 0;
    clay_voxel_grid* back = nullptr;
    CHECK(clay_document_voxel_layer(c.d, "blocks", &found, &back) == CLAY_ERROR_NOT_FOUND);
    CHECK(c.depth() == before);

    CHECK(c.redo());
    CHECK(c.layers() == 2);
    REQUIRE(clay_document_voxel_layer(c.d, "blocks", &found, &back) == CLAY_OK);
    CHECK(found == vox);
    CHECK(occupied(back) == filled);  // and the cells came back with it
}

TEST_CASE("c abi: an ungrouped crossing is two steps, in the order it happened") {
    // The contract for a host that does NOT bracket, pinned so the change
    // above is understood to be about grouping and nothing else. Two edits the
    // host did not bundle stay two undos, newest first.
    Crossing c;
    clay_layer_id vox = 0;
    clay_voxel_grid* g = c.fill(&vox);
    const std::size_t filled = occupied(g);
    CHECK(filled > 0);
    CHECK(c.depth() == 2);

    CHECK(c.undo());  // the fill
    CHECK(c.layers() == 2);
    CHECK(occupied(g) == 0);

    CHECK(c.undo());  // then the layer
    CHECK(c.layers() == 1);
}

TEST_CASE("c abi: an undone crossing saves nothing, and its id is reusable") {
    // Undoing the creation deliberately KEEPS the cells, so a redo can pick
    // them back up — which means the document holds a grid for a layer it does
    // not have. That is fine in memory and must not reach a file: layer ids are
    // derived from the layers PRESENT when a document is loaded, so a saved
    // orphan can be captured by the next layer to take the id and come up
    // holding a dead sculpt. (add-mesh-layers task 7.7, for the voxel side.)
    Crossing c;
    REQUIRE(clay_document_begin_undo_group(c.d) == CLAY_OK);
    clay_layer_id vox = 0;
    clay_voxel_grid* g = c.fill(&vox);
    REQUIRE(clay_document_end_undo_group(c.d) == CLAY_OK);
    CHECK(occupied(g) > 0);
    CHECK(c.undo());

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(c.d, &blob) == CLAY_OK);
    const std::uint8_t* bytes = clay_blob_data(blob);
    const std::size_t size = clay_blob_size(blob);
    REQUIRE(bytes != nullptr);
    clay_document* reloaded = nullptr;
    REQUIRE(clay_document_load_memory(bytes, size, &reloaded) == CLAY_OK);
    clay_blob_destroy(blob);

    std::size_t layers = 0;
    REQUIRE(clay_document_layer_count(reloaded, &layers) == CLAY_OK);
    CHECK(layers == 1);
    clay_layer_id found = 0;
    clay_voxel_grid* back = nullptr;
    CHECK(clay_document_voxel_layer(reloaded, "blocks", &found, &back) == CLAY_ERROR_NOT_FOUND);

    // The corruption this guards: a fresh voxel layer in the reloaded document
    // must be EMPTY, whatever id it is given.
    clay_layer_id fresh = 0;
    clay_voxel_grid* fresh_grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(reloaded, "new", 0.05f, &fresh, &fresh_grid) == CLAY_OK);
    CHECK(occupied(fresh_grid) == 0);

    clay_document_destroy(reloaded);
}

TEST_CASE("c abi: an undone voxel layer is not reachable by its id either") {
    // #365. The grid deliberately OUTLIVES the layer across an undo, so that a
    // redo can pick the cells back up. That is exactly what makes an
    // id-addressed lookup dangerous if it resolves the id in the grids held
    // beside the document: it would hand back the grid of a layer that is not
    // in the document, which is a state the by-name lookup reports as
    // NOT_FOUND. The two have to agree, so the id is resolved in the DOCUMENT.
    clay_document* d = clay_document_create();
    REQUIRE(d != nullptr);
    REQUIRE(clay_document_enable_undo(d) == CLAY_OK);

    // The crossing the header recommends bracketing: one step covering the
    // layer and the fill, so a single undo takes back what the user asked to.
    clay_layer_id id = 0;
    clay_voxel_grid* g = nullptr;
    const std::int32_t cell[3] = {2, 3, 4};
    REQUIRE(clay_document_begin_undo_group(d) == CLAY_OK);
    REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &id, &g) == CLAY_OK);
    REQUIRE(clay_voxel_set(g, cell, 5) == CLAY_OK);
    REQUIRE(clay_document_end_undo_group(d) == CLAY_OK);

    clay_voxel_grid* by_id = nullptr;
    REQUIRE(clay_document_voxel_layer_by_id(d, id, &by_id) == CLAY_OK);
    CHECK(by_id == g);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(d, &undone) == CLAY_OK);
    CHECK(undone == 1);
    clay_voxel_grid* after = nullptr;
    CHECK(clay_document_voxel_layer_by_id(d, id, &after) == CLAY_ERROR_NOT_FOUND);
    CHECK(after == nullptr);  // a refused lookup writes nothing
    // The same answer the name gives, which is the agreement being asserted.
    clay_layer_id found = 0;
    CHECK(clay_document_voxel_layer(d, "blocks", &found, &after) == CLAY_ERROR_NOT_FOUND);

    // Redo brings the layer back under its own id, with the cells the undo kept.
    std::int32_t redone = 0;
    REQUIRE(clay_document_redo(d, &redone) == CLAY_OK);
    CHECK(redone == 1);
    REQUIRE(clay_document_voxel_layer_by_id(d, id, &after) == CLAY_OK);
    std::int32_t read = 0;
    REQUIRE(clay_voxel_get(after, cell, &read) == CLAY_OK);
    CHECK(read == 5);

    clay_document_destroy(d);
}
