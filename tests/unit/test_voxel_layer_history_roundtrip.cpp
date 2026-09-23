// A voxel sculpt-layer operation undoes, redoes and undoes AGAIN to the same
// bytes, on a grid with two resolution levels.
//
// The companion file checks each operation once in each direction on a
// single-level ball. This one runs the full undo -> redo -> undo cycle, on a
// fixture that differs in the ways that could break it: the verbs act on a
// FINE level, so every write propagates to its parent; the passes use smooth
// and pinch as well as inflate; a layer sits at a fractional strength, so the
// recompose runs through the dither; and a plain edit outside any layer sits
// between the passes and the operation, which a recompose-based undo would
// clobber. `serialize()` covers every level and the whole stack.

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "clay/scene/document.h"
#include "clay/session/history.h"
#include "clay/voxel/grid.h"

using namespace clay;
using voxel::VoxelGrid;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Op = VoxelGrid::SculptLayerOp;

voxel::BrushParams brush(int size) {
    voxel::BrushParams p;
    p.size = size;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = voxel::BrushFalloff::Constant;
    p.strength = 1.0f;
    return p;
}

// A solid box of coarse cells, `half` either side of the origin.
void fill_box(VoxelGrid& grid, int half) {
    const std::uint8_t idx = grid.palette_add(kernel::cf3(0.5f, 0.4f, 0.3f));
    for (int z = -half; z <= half; ++z)
        for (int y = -half; y <= half; ++y)
            for (int x = -half; x <= half; ++x) grid.set({x, y, z}, idx);
}

struct Fine {
    scene::Document doc;
    VoxelGrid grid{0.2f};
    scene::LayerId layer = 3;
    session::History h;

    Fine() { REQUIRE(set_up()); }
    // Plain bools rather than doctest macros, so the set-up reads as one
    // precondition and the fixture stays inside the complexity target.
    bool set_up() {
        fill_box(grid, 4);
        if (grid.add_level() != 1 || !grid.set_active_level(1)) return false;
        h.set_enabled(true);
        const bool passes =
            pass("inflate", [this] { grid.sculpt_inflate({0, 9, 0}, brush(7), 2); }) &&
            pass("pinch", [this] { grid.sculpt_pinch({0, 9, 1}, brush(7)); }) &&
            pass("smooth", [this] { grid.sculpt_smooth({2, 8, 0}, brush(9)); });
        // Set-up, unrecorded: the middle layer sits in the dither.
        if (!passes || !grid.set_sculpt_layer_strength(1, 0.6f)) return false;
        // A write outside every layer, over cells the passes also wrote.
        if (!h.begin_voxel_step(layer, grid)) return false;
        for (int x = -2; x <= 2; ++x) grid.set({x, 9, 0}, 0);
        h.end_voxel_step(grid);
        return grid.sculpt_layer_count() == 3 && h.undo_depth() == 4;
    }
    bool pass(const char* name, const std::function<void()>& verb) {
        grid.begin_sculpt_layer(name);
        if (!h.begin_voxel_step(layer, grid)) return false;
        verb();
        h.end_voxel_step(grid);
        grid.end_sculpt_layer();
        return true;
    }
    session::History::GridFor grid_for() {
        return [this](scene::LayerId id) -> VoxelGrid* { return id == layer ? &grid : nullptr; };
    }
    session::History::MeshFor mesh_for() {
        return [](scene::LayerId) -> mesh::Mesh* { return nullptr; };
    }
    bool undo() { return h.undo(doc, grid_for(), mesh_for()); }
    bool redo() { return h.redo(doc, grid_for(), mesh_for()); }
};

// One direction of the walk: undo or redo, then compare every byte.
bool step_to(Fine& f, bool forward, const Bytes& expected) {
    const bool applied = forward ? f.redo() : f.undo();
    return applied && f.grid.serialize() == expected;
}

// Undo -> redo -> undo, landing on `before`, `after`, `before`.
bool walk(Fine& f, const Bytes& before, const Bytes& after) {
    return step_to(f, false, before) && step_to(f, true, after) && step_to(f, false, before);
}

// Run one operation, record it, and walk undo -> redo -> undo, checking the
// bytes at every stop and that the walk ends one step shallower than it began.
// Returns the op so a caller can check its shape.
Op cycle(Fine& f, const std::function<bool(Op*)>& operation) {
    const Bytes before = f.grid.serialize();
    const std::size_t depth = f.h.undo_depth();
    Op op;
    const bool ran = operation(&op) && !op.empty();
    const Bytes after = f.grid.serialize();
    REQUIRE((ran && before != after));
    f.h.record_voxel_layer_property(f.layer, op);
    const bool recorded = f.h.undo_depth() == depth + 1;
    CHECK((recorded && walk(f, before, after) && f.h.undo_depth() == depth));
    return op;
}

}  // namespace

TEST_CASE("voxel layer round trip: strength, on a fine level through the dither") {
    Fine f;
    const Op op = cycle(f, [&](Op* r) { return f.grid.set_sculpt_layer_strength(0, 0.35f, r); });
    CHECK(op.kind == Op::Kind::Strength);
    CHECK(f.grid.sculpt_layer_strength(0) == 1.0f);
}

TEST_CASE("voxel layer round trip: visibility of a dialled middle layer") {
    Fine f;
    const Op op = cycle(f, [&](Op* r) { return f.grid.set_sculpt_layer_visible(1, false, r); });
    CHECK(op.kind == Op::Kind::Visible);
    CHECK(f.grid.sculpt_layer_visible(1));
}

TEST_CASE("voxel layer round trip: moving the bottom layer to the top") {
    Fine f;
    const Op op = cycle(f, [&](Op* r) { return f.grid.move_sculpt_layer(0, 2, r); });
    CHECK(op.kind == Op::Kind::Move);
    CHECK(f.grid.sculpt_layer_name(0) == "inflate");
}

TEST_CASE("voxel layer round trip: removing the dialled middle layer") {
    Fine f;
    const Op op = cycle(f, [&](Op* r) { return f.grid.remove_sculpt_layer(1, r); });
    CHECK(op.kind == Op::Kind::Remove);
    CHECK(f.grid.sculpt_layer_strength(1) == 0.6f);
}

TEST_CASE("voxel layer round trip: merging the top layer down onto the dialled one") {
    Fine f;
    const Op op = cycle(f, [&](Op* r) { return f.grid.merge_sculpt_layer_down(2, r); });
    CHECK(op.kind == Op::Kind::Merge);
    CHECK(f.grid.sculpt_layer_count() == 3);
    CHECK(f.grid.sculpt_layer_name(2) == "smooth");
}

namespace {

// Undo everything back to states[0], then redo to the end, comparing each stop.
bool unwind_and_rewind(Fine& f, const std::vector<Bytes>& states) {
    for (std::size_t i = states.size() - 1; i > 0; --i)
        if (!step_to(f, false, states[i - 1])) return false;
    for (std::size_t i = 1; i < states.size(); ++i)
        if (!step_to(f, true, states[i])) return false;
    return true;
}

// Run and record each operation in turn, keeping the bytes after each one
// (states[0] is before the first). Empty when any operation is refused.
std::vector<Bytes> record_all(Fine& f, const std::vector<std::function<bool(Op*)>>& ops) {
    std::vector<Bytes> states{f.grid.serialize()};
    for (const auto& operation : ops) {
        Op op;
        if (!operation(&op) || op.empty()) return {};
        f.h.record_voxel_layer_property(f.layer, op);
        states.push_back(f.grid.serialize());
    }
    return states;
}

}  // namespace

TEST_CASE("voxel layer round trip: all five in a row, unwound and rewound twice") {
    Fine f;
    const std::vector<Bytes> states = record_all(
        f, {
               [&](Op* r) { return f.grid.set_sculpt_layer_strength(2, 0.5f, r); },
               [&](Op* r) { return f.grid.move_sculpt_layer(2, 0, r); },
               [&](Op* r) { return f.grid.set_sculpt_layer_visible(2, false, r); },
               [&](Op* r) { return f.grid.merge_sculpt_layer_down(1, r); },
               [&](Op* r) { return f.grid.remove_sculpt_layer(0, r); },
           });
    // Three passes, the edit and five operations; then the whole walk twice.
    REQUIRE((states.size() == 6 && f.h.undo_depth() == 9));
    CHECK((unwind_and_rewind(f, states) && unwind_and_rewind(f, states)));
}
