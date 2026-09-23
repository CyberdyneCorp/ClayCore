// A voxel sculpt-layer operation is an undo step (unify-the-undo-history 3.3,
// 3.4, 4.4), and a bracket only folds what it opened (2.4).
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

    // One sculpt-layer pass, recorded the way both bindings record a verb: the
    // pass joins the artist's layer AND becomes one undo step.
    // `also`, when given, is a second inflate inside the same pass.
    void pass(const char* name, voxel::VoxelCoord at, int amount,
              std::optional<voxel::VoxelCoord> also = std::nullopt) {
        grid.begin_sculpt_layer(name);
        REQUIRE(h.begin_voxel_step(voxel_layer, grid));
        grid.sculpt_inflate(at, dab(9), amount);
        if (also) grid.sculpt_inflate(*also, dab(9), 2);
        h.end_voxel_step(grid);
        grid.end_sculpt_layer();
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
    REQUIRE(w.h.undo_depth() == 1);

    VoxelGrid::SculptLayerOp op;
    REQUIRE(w.grid.set_sculpt_layer_strength(0, 0.4f, &op));
    CHECK(op.kind == VoxelGrid::SculptLayerOp::Kind::Strength);
    CHECK_FALSE(op.cells.empty());  // the dial really moved cells
    w.record(op);
    const Bytes dialled = w.grid.serialize();
    CHECK(w.h.undo_depth() == 2);  // the pass, and the dial

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
    CHECK(w.h.step_count() == 1);
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
    // The snapshot already holds both layers. Creating a sculpt layer is not a
    // history event — a journal replayed onto a snapshot taken BEFORE the
    // layers existed rebuilds their cells and not the stack, and refuses the
    // first operation that names a layer it does not have — so the claim here
    // is about the operations, replayed onto the stack they were made on.
    World w;
    w.pass("lower", {0, 6, 0}, 2);
    w.pass("upper", {0, 7, 0}, -1);
    const Bytes snapshot = w.grid.serialize();
    const std::size_t from = w.h.journal_next();
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
    CHECK(result.applied == 2);
    CHECK(r.grid.serialize() == w.grid.serialize());
    // And the rebuilt steps undo like the live ones: the merge, then the dial.
    REQUIRE(r.undo());
    REQUIRE(r.undo());
    CHECK(r.grid.sculpt_layer_strength(1) == 1.0f);
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
