#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "clay.h"

// Voxel sculpt-layer operations as undo steps across the ABI, and what enabling
// undo mid-session does (unify-the-undo-history 3.3, 3.4, 4.4 and 2.4).
//
// Before this, clay_voxel_set_sculpt_layer_strength and its four siblings
// recorded nothing: a host that dialled a pass to 40% and pressed undo got the
// PASS reverted — onto cells the dial had already moved — and the slider left
// at 40%. The comparisons below read the whole document through
// clay_document_save_memory, so "restored" means the grid AND its layer stack.

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id voxels = 0;

    explicit Doc(bool undo) {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        fill_block();
        if (undo) enable();
    }
    void fill_block() {
        clay_voxel_grid* g = nullptr;
        REQUIRE(clay_document_add_voxel_layer(d, "blocks", 0.1f, &voxels, &g) == CLAY_OK);
        const std::int32_t lo[3] = {-6, -6, -6};
        const std::int32_t hi[3] = {6, 6, 6};
        REQUIRE(clay_voxel_fill_box(g, lo, hi, 1) == CLAY_OK);
    }
    void enable() { REQUIRE(clay_document_enable_undo(d) == CLAY_OK); }
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
    Bytes bytes() const {
        clay_blob* blob = nullptr;
        REQUIRE(clay_document_save_memory(d, &blob) == CLAY_OK);
        Bytes out(clay_blob_data(blob), clay_blob_data(blob) + clay_blob_size(blob));
        clay_blob_destroy(blob);
        return out;
    }
    // One recorded pass: a sculpt layer holding one inflate.
    void pass(const char* name, std::int32_t y, std::int32_t amount) {
        size_t index = 0;
        REQUIRE(clay_voxel_begin_sculpt_layer(grid(), name, &index) == CLAY_OK);
        clay_brush_params brush{};
        brush.struct_size = sizeof(brush);
        brush.size = 9;
        brush.shape = 1; /* sphere */
        brush.strength = 1.0f;
        const std::int32_t at[3] = {0, y, 0};
        REQUIRE(clay_voxel_sculpt_inflate(grid(), at, &brush, amount) == CLAY_OK);
        REQUIRE(clay_voxel_end_sculpt_layer(grid()) == CLAY_OK);
    }
    float strength(size_t layer) {
        float s = -1.0f;
        REQUIRE(clay_voxel_sculpt_layer_strength(grid(), layer, &s) == CLAY_OK);
        return s;
    }
    size_t layers() {
        size_t n = 0;
        REQUIRE(clay_voxel_sculpt_layer_count(grid(), &n) == CLAY_OK);
        return n;
    }
    size_t layer_cells(size_t layer) {
        size_t n = 0;
        REQUIRE(clay_voxel_sculpt_layer_cell_count(grid(), layer, &n) == CLAY_OK);
        return n;
    }
    size_t occupied() {
        size_t n = 0;
        REQUIRE(clay_voxel_occupied_count(grid(), &n) == CLAY_OK);
        return n;
    }
    size_t journal_next() const {
        size_t next = 0;
        REQUIRE(clay_document_journal_range(d, nullptr, &next) == CLAY_OK);
        return next;
    }
    Bytes journal_since(size_t from) const {
        clay_blob* blob = nullptr;
        REQUIRE(clay_document_journal_since(d, from, &blob, nullptr) == CLAY_OK);
        Bytes out(clay_blob_data(blob), clay_blob_data(blob) + clay_blob_size(blob));
        clay_blob_destroy(blob);
        return out;
    }
};

// A document's whole bytes, read the way Doc::bytes reads them.
Bytes bytes_of(const clay_document* d) {
    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(d, &blob) == CLAY_OK);
    Bytes out(clay_blob_data(blob), clay_blob_data(blob) + clay_blob_size(blob));
    clay_blob_destroy(blob);
    return out;
}

}  // namespace

TEST_CASE("c abi: undoing a sculpt-layer dial restores the dial, not the pass") {
    // The regression. Before this the dial was not a step, so the undo reached
    // the PASS: its cells were reverted onto a grid the dial had moved, and the
    // slider still read 0.4.
    Doc doc(true);
    doc.pass("wrinkles", 6, 2);
    const Bytes full = doc.bytes();
    const std::size_t cells = doc.layer_cells(0);
    REQUIRE(doc.depth() == 2);  // the layer's creation, and the pass inside it

    REQUIRE(clay_voxel_set_sculpt_layer_strength(doc.grid(), 0, 0.4f) == CLAY_OK);
    const Bytes dialled = doc.bytes();
    CHECK(doc.depth() == 3);

    CHECK(doc.undo());
    CHECK(doc.strength(0) == 1.0f);
    CHECK(doc.layers() == 1);
    CHECK(doc.layer_cells(0) == cells);
    CHECK(doc.bytes() == full);

    CHECK(doc.redo());
    CHECK(doc.strength(0) == doctest::Approx(0.4f));
    CHECK(doc.bytes() == dialled);
}

TEST_CASE("c abi: undoing a merge-down restores both layers") {
    Doc doc(true);
    doc.pass("lower", 6, 2);
    doc.pass("upper", 8, 2);  // overlaps the lower pass AND reaches past it
    const Bytes before = doc.bytes();
    const std::size_t lower = doc.layer_cells(0);
    const std::size_t upper = doc.layer_cells(1);

    REQUIRE(clay_voxel_merge_sculpt_layer_down(doc.grid(), 1) == CLAY_OK);
    const Bytes merged = doc.bytes();
    REQUIRE(doc.layers() == 1);

    CHECK(doc.undo());
    CHECK(doc.layers() == 2);
    CHECK(doc.layer_cells(0) == lower);
    CHECK(doc.layer_cells(1) == upper);
    CHECK(doc.bytes() == before);
    CHECK(doc.redo());
    CHECK(doc.bytes() == merged);
}

TEST_CASE("c abi: visibility, reorder and removal are each one step") {
    Doc doc(true);
    doc.pass("a", 6, 2);
    doc.pass("b", 6, -2);
    const std::size_t base = doc.depth();
    REQUIRE(clay_voxel_set_sculpt_layer_visible(doc.grid(), 1, 0) == CLAY_OK);
    REQUIRE(clay_voxel_move_sculpt_layer(doc.grid(), 1, 0) == CLAY_OK);
    REQUIRE(clay_voxel_remove_sculpt_layer(doc.grid(), 0) == CLAY_OK);
    CHECK(doc.depth() == base + 3);
    // A refused merge records nothing.
    CHECK(clay_voxel_merge_sculpt_layer_down(doc.grid(), 0) != CLAY_OK);
    CHECK(doc.depth() == base + 3);
}

// -- creating a sculpt layer is a step (#642) --------------------------------

TEST_CASE("c abi: undoing a pass inside a sculpt layer takes it out of the layer") {
    // Regression (#642). The pass's cells were a step and the layer's record of
    // them was not, so the undo reverted the cells and left the record listing
    // them — and the next dial recomposed the layer and put the undone cells
    // back, at the dithered fraction.
    Doc doc(true);
    size_t index = 0;
    REQUIRE(clay_voxel_begin_sculpt_layer(doc.grid(), "wrinkles", &index) == CLAY_OK);
    const Bytes opened = doc.bytes();
    const std::size_t cells = doc.occupied();
    clay_brush_params brush{};
    brush.struct_size = sizeof(brush);
    brush.size = 9;
    brush.shape = 1; /* sphere */
    brush.strength = 1.0f;
    const std::int32_t at[3] = {0, 6, 0};
    REQUIRE(clay_voxel_sculpt_inflate(doc.grid(), at, &brush, 2) == CLAY_OK);
    REQUIRE(doc.layer_cells(0) > 0);
    REQUIRE(doc.occupied() > cells);

    CHECK(doc.undo());
    CHECK(doc.layer_cells(0) == 0);
    CHECK(doc.bytes() == opened);
    // The half a host could see: dialling the now-empty layer moves nothing.
    REQUIRE(clay_voxel_set_sculpt_layer_strength(doc.grid(), 0, 0.5f) == CLAY_OK);
    CHECK(doc.occupied() == cells);
    // Still recording: undoing a dab mid-pass does not end the pass.
    int32_t recording = 0;
    REQUIRE(clay_voxel_recording_sculpt_layer(doc.grid(), &recording) == CLAY_OK);
    CHECK(recording == 1);
}

TEST_CASE("c abi: undoing a sculpt layer's creation removes it, and redo brings the pass back") {
    Doc doc(true);
    const Bytes start = doc.bytes();
    doc.pass("wrinkles", 6, 2);
    const Bytes made = doc.bytes();
    const std::size_t cells = doc.layer_cells(0);
    REQUIRE(doc.depth() == 2);

    CHECK(doc.undo());  // the pass
    CHECK(doc.undo());  // the layer
    CHECK(doc.layers() == 0);
    CHECK(doc.bytes() == start);
    CHECK_FALSE(doc.undo());

    CHECK(doc.redo());
    CHECK(doc.layers() == 1);
    CHECK(doc.layer_cells(0) == 0);
    CHECK(doc.redo());
    CHECK(doc.layer_cells(0) == cells);
    CHECK(doc.bytes() == made);
}

TEST_CASE("c abi: a journal replayed onto a snapshot older than the layer rebuilds the stack") {
    // Regression (#642). Creation was not an event, so a snapshot taken before
    // the layer existed rebuilt the pass's cells and not the layer, and the
    // replay was refused at the first dial naming it.
    Doc doc(true);
    const Bytes snapshot = doc.bytes();
    const std::size_t from = doc.journal_next();
    doc.pass("lower", 6, 2);
    doc.pass("upper", 8, -1);
    REQUIRE(clay_voxel_set_sculpt_layer_strength(doc.grid(), 1, 0.3f) == CLAY_OK);
    const Bytes journal = doc.journal_since(from);
    const std::size_t next = doc.journal_next();
    REQUIRE(next - from == 5);  // two creations, two passes, the dial

    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(snapshot.data(), snapshot.size(), &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    std::size_t applied = 0;
    int32_t stopped = -1;
    CHECK(clay_document_replay_journal(recovered, journal.data(), journal.size(), &applied,
                                       &stopped) == CLAY_OK);
    CHECK(applied == next - from);
    CHECK(stopped == 0);
    CHECK(bytes_of(recovered) == doc.bytes());
    clay_document_destroy(recovered);
}

// -- enabling undo mid-session (2.4) -----------------------------------------

TEST_CASE("c abi: enabling undo mid-session starts an empty history") {
    // DECIDED: enabling is a light switch that starts recording from here, and
    // is never refused — the SDF path's meaning of enable_undo is unchanged.
    Doc doc(false);
    doc.pass("before", 6, 2);  // made while undo was off
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    CHECK(doc.depth() == 0);    // what came before is the starting state
    CHECK_FALSE(doc.undo());

    // A second enable keeps the history it already has.
    REQUIRE(clay_voxel_set_sculpt_layer_strength(doc.grid(), 0, 0.5f) == CLAY_OK);
    REQUIRE(doc.depth() == 1);
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    CHECK(doc.depth() == 1);
}

TEST_CASE("c abi: a gesture's end after a mid-gesture enable folds nothing") {
    // Regression. begin_undo_group is refused while undo is off, so a host
    // that enables undo mid-gesture reaches the gesture's end with no bracket
    // open — and that end folded every step since the session began into one.
    Doc doc(false);
    CHECK(clay_document_begin_undo_group(doc.d) != CLAY_OK);
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);
    const std::int32_t a[3] = {20, 0, 0};
    const std::int32_t b[3] = {21, 0, 0};
    REQUIRE(clay_voxel_set(doc.grid(), a, 1) == CLAY_OK);
    REQUIRE(clay_voxel_set(doc.grid(), b, 1) == CLAY_OK);
    REQUIRE(clay_document_end_undo_group(doc.d) == CLAY_OK);
    CHECK(doc.depth() == 2);
}
