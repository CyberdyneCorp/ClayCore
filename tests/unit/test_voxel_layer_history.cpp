// A voxel sculpt-layer operation is an undo step (unify-the-undo-history 3.3,
// 3.4, 4.4), and so are a layer's creation and each edit made inside it
// (#642); a bracket only folds what it opened (2.4).
//
// The claim is EXACT, not close. A layer operation's step carries the property
// it changed AND every cell its recompose wrote, so an undo restores the grid
// and its stack to the bytes they had — `serialize()` covers both, and is what
// every comparison here reads. Before this, the five operations were not steps
// at all: a dial moved cells under the history without telling it, and the
// next undo replayed an older step onto a grid that no longer matched it.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "clay/scene/commands.h"
#include "clay/session/history.h"
#include "clay/voxel/grid.h"

using namespace clay;
using voxel::VoxelGrid;

namespace {

using Bytes = std::vector<std::uint8_t>;

scene::Command add_sphere(scene::LayerId layer, float r) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    scene::AddNodeCmd cmd;
    cmd.layer = layer;
    cmd.subtree.push_back(n);
    return scene::Command{cmd};
}

voxel::BrushParams dab(int size) {
    voxel::BrushParams p;
    p.size = size;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = voxel::BrushFalloff::Constant;
    p.strength = 1.0f;
    return p;
}

// A document, one voxel layer and a history over them. The base ball is laid
// down BEFORE the history is enabled, so it is the starting state rather than
// a step.
struct World {
    scene::Document doc;
    scene::LayerId sdf_layer = 0;
    VoxelGrid grid{0.1f};
    scene::LayerId voxel_layer = 7;
    session::History h;

    World() {
        sdf_layer = doc.add_sdf_layer("body").id;
        const std::uint8_t idx = grid.palette_add(kernel::cf3(0.6f, 0.6f, 0.6f));
        for (int z = -6; z <= 6; ++z)
            for (int y = -6; y <= 6; ++y)
                for (int x = -6; x <= 6; ++x)
                    if (x * x + y * y + z * z <= 36) grid.set({x, y, z}, idx);
        h.set_enabled(true);
    }
    session::History::GridFor grid_for() {
        return [this](scene::LayerId id) -> VoxelGrid* { return id == voxel_layer ? &grid : nullptr; };
    }
    session::History::MeshFor mesh_for() {
        return [](scene::LayerId) -> mesh::Mesh* { return nullptr; };
    }
    bool undo() { return h.undo(doc, grid_for(), mesh_for()); }
    bool redo() { return h.redo(doc, grid_for(), mesh_for()); }

    // One sculpt-layer pass, recorded the way both bindings record it: the
    // layer's creation is one undo step, and the edit inside it another.
    // `also`, when given, is a second inflate inside the same edit.
    void pass(const char* name, voxel::VoxelCoord at, int amount,
              std::optional<voxel::VoxelCoord> also = std::nullopt) {
        open(name);
        inflate(at, amount, also);
        grid.end_sculpt_layer();
    }
    void open(const char* name) {
        VoxelGrid::SculptLayerOp begin;
        grid.begin_sculpt_layer(name, &begin);
        record(begin);
    }
    // One bracketed edit, the way both bindings bracket a verb.
    void inflate(voxel::VoxelCoord at, int amount,
                 std::optional<voxel::VoxelCoord> also = std::nullopt) {
        REQUIRE(h.begin_voxel_step(voxel_layer, grid));
        grid.sculpt_inflate(at, dab(9), amount);
        if (also) grid.sculpt_inflate(*also, dab(9), 2);
        h.end_voxel_step(grid);
    }
    void record(const VoxelGrid::SculptLayerOp& op) { h.record_voxel_layer_property(voxel_layer, op); }
};

void undo_to(World& w, const Bytes& expected) {
    REQUIRE(w.undo());
    CHECK(w.grid.serialize() == expected);
}
void redo_to(World& w, const Bytes& expected) {
    REQUIRE(w.redo());
    CHECK(w.grid.serialize() == expected);
}
// Undo, check the bytes are the ones from before; redo, check they are the
// ones from after. Both directions, one call, so no test forgets redo.
void round_trip(World& w, const Bytes& before, const Bytes& after) {
    undo_to(w, before);
    redo_to(w, after);
}

}  // namespace

TEST_CASE("voxel layer history: a strength change is its own step, and undoes bit-exact") {
    World w;
    w.pass("wrinkles", {0, 6, 0}, 2);
    const Bytes full = w.grid.serialize();
    REQUIRE(w.h.undo_depth() == 2);  // the layer's creation, and the pass

    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.set_sculpt_layer_strength(0, 0.4f, &op));
    CHECK(op.kind == VoxelGrid::SculptLayerOp::Kind::Strength);
    CHECK_FALSE(op.cells.empty());  // the dial really moved cells
    w.record(op);
    const Bytes dialled = w.grid.serialize();
    CHECK(w.h.undo_depth() == 3);  // the creation, the pass, and the dial

    round_trip(w, full, dialled);
    // The undo took the DIAL, not the pass: one undo leaves the layer present,
    // at full strength, with its cells.
    REQUIRE(w.undo());
    CHECK(w.grid.sculpt_layer_count() == 1);
    CHECK(w.grid.sculpt_layer_strength(0) == 1.0f);
    CHECK(w.grid.serialize() == full);
}

TEST_CASE("voxel layer history: visibility undoes and redoes bit-exact") {
    World w;
    w.pass("a", {0, 6, 0}, 2);
    const Bytes shown = w.grid.serialize();
    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.set_sculpt_layer_visible(0, false, &op));
    w.record(op);
    const Bytes hidden = w.grid.serialize();
    REQUIRE(shown != hidden);
    round_trip(w, shown, hidden);
    CHECK_FALSE(w.grid.sculpt_layer_visible(0));
}

TEST_CASE("voxel layer history: a reorder undoes and redoes bit-exact") {
    World w;
    // Overlapping passes, so the order decides which value survives and a
    // reorder really moves cells.
    w.pass("inflate", {0, 6, 0}, 2);
    w.pass("carve", {0, 6, 0}, -2);
    const Bytes before = w.grid.serialize();
    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.move_sculpt_layer(1, 0, &op));
    w.record(op);
    const Bytes after = w.grid.serialize();
    REQUIRE(before != after);
    round_trip(w, before, after);
    CHECK(w.grid.sculpt_layer_name(0) == "carve");
}

TEST_CASE("voxel layer history: undoing a removal puts the pass back") {
    World w;
    w.pass("a", {0, 6, 0}, 2);
    w.pass("b", {6, 0, 0}, 2);
    const Bytes before = w.grid.serialize();
    const std::size_t cells = w.grid.sculpt_layer_cell_count(0);
    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.remove_sculpt_layer(0, &op));
    w.record(op);
    const Bytes after = w.grid.serialize();
    REQUIRE(w.grid.sculpt_layer_count() == 1);

    REQUIRE(w.undo());
    CHECK(w.grid.sculpt_layer_count() == 2);
    CHECK(w.grid.sculpt_layer_name(0) == "a");
    CHECK(w.grid.sculpt_layer_cell_count(0) == cells);
    CHECK(w.grid.serialize() == before);
    REQUIRE(w.redo());
    CHECK(w.grid.serialize() == after);
}

TEST_CASE("voxel layer history: undoing a merge-down restores BOTH layers") {
    World w;
    // Overlapping, so the fold overwrites afters in the lower layer — the part
    // of a merge that is not an append and that an undo has to put back.
    w.pass("lower", {0, 6, 0}, 2);
    // The upper pass carves into cells the lower one wrote AND inflates
    // somewhere the lower never reached.
    w.pass("upper", {0, 7, 0}, -1, voxel::VoxelCoord{5, 3, 0});
    REQUIRE(w.grid.set_sculpt_layer_strength(1, 0.5f));  // unrecorded: set-up only
    const Bytes before = w.grid.serialize();
    const std::size_t lower = w.grid.sculpt_layer_cell_count(0);
    const std::size_t upper = w.grid.sculpt_layer_cell_count(1);

    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.merge_sculpt_layer_down(1, &op));
    // Both halves of a fold really were exercised: cells the lower layer
    // already owned (afters overwritten) and cells it did not (appended).
    CHECK_FALSE(op.lower_afters.empty());
    CHECK(op.held.changes.size() > op.lower_afters.size());
    w.record(op);
    const Bytes merged = w.grid.serialize();
    REQUIRE(w.grid.sculpt_layer_count() == 1);

    REQUIRE(w.undo());
    CHECK(w.grid.sculpt_layer_count() == 2);
    CHECK(w.grid.sculpt_layer_cell_count(0) == lower);
    CHECK(w.grid.sculpt_layer_cell_count(1) == upper);
    CHECK(w.grid.sculpt_layer_strength(1) == 0.5f);
    CHECK(w.grid.serialize() == before);
    REQUIRE(w.redo());
    CHECK(w.grid.serialize() == merged);
}

TEST_CASE("voxel layer history: an operation that changed nothing is not a step") {
    World w;
    w.pass("a", {0, 6, 0}, 2);
    VoxelGrid::SculptLayerOp same, onto_itself;
    REQUIRE(w.grid.set_sculpt_layer_strength(0, 1.0f, &same));
    REQUIRE(w.grid.move_sculpt_layer(0, 0, &onto_itself));
    CHECK(same.empty());
    CHECK(onto_itself.empty());
    w.record(same);
    w.record(onto_itself);
    CHECK(w.h.step_count() == 2);  // the creation and the pass, nothing more
}

TEST_CASE("voxel layer history: a step whose stack changed shape is refused, not misapplied") {
    World w;
    w.pass("lower", {0, 6, 0}, 2);
    w.pass("upper", {0, 7, 0}, -1);
    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.merge_sculpt_layer_down(1, &op));
    // Behind the history's back the stack loses the layer the record names.
    REQUIRE(w.grid.remove_sculpt_layer(0));
    const Bytes now = w.grid.serialize();
    CHECK_FALSE(w.grid.apply_sculpt_layer_op(op, /*forward=*/false));
    CHECK(w.grid.serialize() == now);  // refused before a single cell moved
}

TEST_CASE("voxel layer history: a dialled pass replays from the journal") {
    // The snapshot is taken BEFORE either layer exists. Until creation was a
    // history event (#642) this replay rebuilt the passes' cells and not the
    // stack, and refused the dial — the first event naming a layer — so this
    // test used to snapshot after the layers and claim only the operations.
    World w;
    const Bytes snapshot = w.grid.serialize();
    const std::size_t from = w.h.journal_next();
    w.pass("lower", {0, 6, 0}, 2);
    w.pass("upper", {0, 7, 0}, -1);
    VoxelGrid::SculptLayerOp dial, merge;
    REQUIRE(w.grid.set_sculpt_layer_strength(1, 0.3f, &dial));
    w.record(dial);
    REQUIRE(w.grid.merge_sculpt_layer_down(1, &merge));
    w.record(merge);
    const Bytes journal = w.h.journal_since(from, nullptr);

    World r;  // the crash
    std::optional<VoxelGrid> restored = VoxelGrid::deserialize(snapshot.data(), snapshot.size());
    REQUIRE(restored);
    r.grid = std::move(*restored);
    session::History::ReplayResult result;
    REQUIRE(r.h.replay(journal.data(), journal.size(), r.doc, r.grid_for(), r.mesh_for(), &result));
    CHECK(result.applied == 6);  // two creations, two passes, the dial, the merge
    CHECK(r.grid.serialize() == w.grid.serialize());
    // And the rebuilt steps undo like the live ones: the merge, then the dial.
    REQUIRE(r.undo());
    REQUIRE(r.undo());
    CHECK(r.grid.sculpt_layer_strength(1) == 1.0f);
    // ...and on, back through both passes and both creations to the snapshot.
    for (int i = 0; i < 4; ++i) REQUIRE(r.undo());
    CHECK(r.grid.sculpt_layer_count() == 0);
    CHECK(r.grid.serialize() == snapshot);
}

// -- a layer's creation, and the edits inside it (#642) -----------------------

TEST_CASE("voxel layer history: undoing an edit inside a layer takes it out of the record") {
    // Regression (#642). The edit's cells were a step and the layer's record of
    // them was not: the undo reverted the cells and left the record listing
    // them, so the next dial recomposed the layer and put them back.
    World w;
    w.open("wrinkles");
    const Bytes opened = w.grid.serialize();
    const std::size_t occupied = w.grid.occupied_count();
    w.inflate({0, 6, 0}, 2);
    REQUIRE(w.grid.sculpt_layer_cell_count(0) > 0);
    const Bytes inflated = w.grid.serialize();

    REQUIRE(w.undo());
    CHECK(w.grid.sculpt_layer_cell_count(0) == 0);
    CHECK(w.grid.serialize() == opened);
    CHECK(w.grid.recording_sculpt_layer());  // an undone dab does not end the pass
    REQUIRE(w.redo());
    CHECK(w.grid.serialize() == inflated);

    REQUIRE(w.undo());
    REQUIRE(w.grid.set_sculpt_layer_strength(0, 0.5f));
    CHECK(w.grid.occupied_count() == occupied);
}

TEST_CASE("voxel layer history: an edit that rewrites a pass's own cells undoes bit-exact") {
    // The second edit lands on cells the first already put in the record, so
    // its undo has to restore the `after` those entries held, not only
    // truncate what it appended.
    World w;
    w.open("pass");
    w.inflate({0, 6, 0}, 2);
    const Bytes first = w.grid.serialize();
    const std::size_t cells = w.grid.sculpt_layer_cell_count(0);
    w.inflate({0, 7, 0}, -1, voxel::VoxelCoord{5, 3, 0});
    const Bytes second = w.grid.serialize();
    REQUIRE(second != first);

    round_trip(w, first, second);
    REQUIRE(w.undo());
    CHECK(w.grid.sculpt_layer_cell_count(0) == cells);
    CHECK(w.grid.serialize() == first);
}

TEST_CASE("voxel layer history: an edit inside a layer that changed nothing leaves no trace") {
    // Regression for the fix's own first cut. A dab that changes no cell is
    // dropped rather than recorded, but inside a layer it still lists the cells
    // it touched; left there, every later edit's record named a length the
    // record no longer had once an earlier step was undone, and its redo — and
    // its journal replay, which never saw the dropped dab — was refused.
    World w;
    const Bytes snapshot = w.grid.serialize();
    const std::size_t from = w.h.journal_next();
    w.open("pass");
    w.inflate({0, 6, 0}, 2);
    const Bytes first = w.grid.serialize();
    const std::size_t depth = w.h.undo_depth();

    // Painting a cell the index it already holds changes nothing.
    REQUIRE(w.h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, w.grid.get({0, 0, 0}));
    w.h.end_voxel_step(w.grid);
    CHECK(w.h.undo_depth() == depth);
    CHECK(w.grid.serialize() == first);

    w.inflate({6, 0, 0}, 2);
    const Bytes last = w.grid.serialize();
    REQUIRE(w.undo());
    REQUIRE(w.undo());
    REQUIRE(w.redo());
    REQUIRE(w.redo());
    CHECK(w.grid.serialize() == last);

    World r;
    std::optional<VoxelGrid> restored = VoxelGrid::deserialize(snapshot.data(), snapshot.size());
    REQUIRE(restored);
    r.grid = std::move(*restored);
    const Bytes journal = w.h.journal_since(from, nullptr);
    session::History::ReplayResult result;
    REQUIRE(r.h.replay(journal.data(), journal.size(), r.doc, r.grid_for(), r.mesh_for(), &result));
    CHECK(r.grid.serialize() == last);
}

TEST_CASE("voxel layer history: undoing a creation removes the layer and ends its recording") {
    World w;
    const Bytes start = w.grid.serialize();
    w.open("wrinkles");
    REQUIRE(w.grid.recording_sculpt_layer());
    REQUIRE(w.undo());
    CHECK(w.grid.sculpt_layer_count() == 0);
    CHECK_FALSE(w.grid.recording_sculpt_layer());
    CHECK(w.grid.serialize() == start);
    // Redo brings the layer back CLOSED: recording is a gesture's state, and
    // reopening it would refuse a host's next begin.
    REQUIRE(w.redo());
    CHECK(w.grid.sculpt_layer_count() == 1);
    CHECK(w.grid.sculpt_layer_name(0) == "wrinkles");
    CHECK_FALSE(w.grid.recording_sculpt_layer());
}

TEST_CASE("voxel layer history: a creation with a pass still in it is refused, not misapplied") {
    World w;
    w.pass("a", {0, 6, 0}, 2);
    VoxelGrid::SculptLayerOp begin;
    begin.kind = VoxelGrid::SculptLayerOp::Kind::Begin;
    begin.layer = 0;
    const Bytes now = w.grid.serialize();
    CHECK_FALSE(w.grid.apply_sculpt_layer_op(begin, /*forward=*/false));
    CHECK(w.grid.serialize() == now);
}

TEST_CASE("voxel layer history: the op survives its own encoding") {
    World w;
    w.pass("lower", {0, 6, 0}, 2);
    w.pass("upper", {0, 7, 0}, -1);
    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.merge_sculpt_layer_down(1, &op));
    const Bytes bytes = op.encode();
    VoxelGrid::SculptLayerOp back;
    REQUIRE(VoxelGrid::SculptLayerOp::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.encode() == bytes);
    // Truncated, it is refused rather than half-read.
    CHECK_FALSE(VoxelGrid::SculptLayerOp::decode(bytes.data(), bytes.size() - 1, &back));
}

TEST_CASE("voxel layer history: a pass op survives its own encoding") {
    World w;
    w.grid.begin_sculpt_layer("pass");
    w.grid.sculpt_inflate({0, 6, 0}, dab(9), 2);
    VoxelGrid::SculptLayerOp op;
    w.grid.begin_pass_capture(&op);
    w.grid.sculpt_inflate({0, 7, 0}, dab(9), -1);
    w.grid.end_pass_capture();
    REQUIRE(op.kind == VoxelGrid::SculptLayerOp::Kind::Pass);
    REQUIRE_FALSE(op.pass_afters.empty());  // the rewrite half is exercised
    const Bytes bytes = op.encode();
    VoxelGrid::SculptLayerOp back;
    REQUIRE(VoxelGrid::SculptLayerOp::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.encode() == bytes);
}

// -- brackets (2.4) ----------------------------------------------------------

TEST_CASE("session: an end with no open bracket folds nothing") {
    // Regression. end_group used to fold every step from the last bracket's
    // start — here, from the first step of the session — into ONE step. A host
    // reaches it by enabling undo mid-gesture: its begin_undo_group was refused
    // while undo was off, and the gesture's end then took the history with it.
    World w;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(w.h.begin_voxel_step(w.voxel_layer, w.grid));
        w.grid.set({20 + i, 0, 0}, 1);
        w.h.end_voxel_step(w.grid);
    }
    w.h.end_group();
    CHECK(w.h.undo_depth() == 3);
}

TEST_CASE("session: a nested bracket folds once, at the outermost") {
    // Regression. The inner begin_group moved the fold's start past the steps
    // the OUTER bracket had already collected, so the outer bracket undid in
    // two: its first voxel step on its own, and the rest as one.
    World w;
    w.h.begin_group();
    REQUIRE(w.h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({20, 0, 0}, 1);
    w.h.end_voxel_step(w.grid);
    w.h.begin_group();
    REQUIRE(w.h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f)));
    w.h.end_group();
    REQUIRE(w.h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({21, 0, 0}, 1);
    w.h.end_voxel_step(w.grid);
    w.h.end_group();

    CHECK(w.h.undo_depth() == 1);
    REQUIRE(w.undo());
    CHECK(w.grid.get({20, 0, 0}) == 0);
    CHECK(w.grid.get({21, 0, 0}) == 0);
    CHECK(w.doc.layers[0].sdf->roots.empty());
}
