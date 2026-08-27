#include <doctest/doctest.h>

#include "clay/session/history.h"
#include "clay/voxel/grid.h"

// Bounding the history (add-history-budget).
//
// It had no cap of any kind: no depth limit, no byte accounting, no eviction,
// no query. The only lever was enable_undo, which is not a lever, it is a light
// switch. That was survivable while the history held SDF edits alone; it now
// holds four step kinds and a journal.

using namespace clay;

namespace {

scene::Command add_sphere(scene::LayerId layer, float r) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    scene::AddNodeCmd cmd;
    cmd.layer = layer;
    cmd.subtree.push_back(n);
    return scene::Command{cmd};
}

struct World {
    scene::Document doc;
    scene::LayerId sdf = 0;
    voxel::VoxelGrid grid{0.1f};
    scene::LayerId voxel_layer = 7;
    World() { sdf = doc.add_sdf_layer("body").id; }
    session::History::GridFor grid_for() {
        return [this](scene::LayerId id) -> voxel::VoxelGrid* {
            return id == voxel_layer ? &grid : nullptr;
        };
    }
    session::History::MeshFor mesh_for() {
        return [](scene::LayerId) -> mesh::Mesh* { return nullptr; };
    }
};

// A voxel step of a known size, which is the cheapest way to make the history
// grow by a predictable amount.
void voxel_step(session::History& h, World& w, int base, int cells) {
    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    for (int i = 0; i < cells; ++i) w.grid.set({base + i, 0, 0}, 1);
    h.end_voxel_step(w.grid);
}

}  // namespace

TEST_CASE("history budget: the history reports what it holds") {
    World w;
    session::History h;
    h.set_enabled(true);
    const session::History::Bytes empty = h.bytes();
    CHECK(empty.total == 0);

    voxel_step(h, w, 0, 100);
    const session::History::Bytes one = h.bytes();
    CHECK(one.undo > 0);
    CHECK(one.undo_steps == 1);
    // The journal keeps its own copy of the payload, so a session with crash
    // recovery on holds roughly twice what one without it does. Reported
    // separately, because that is a cost a host can act on.
    CHECK(one.journal > 0);
    CHECK(one.total == one.undo + one.redo + one.journal);

    voxel_step(h, w, 1000, 100);
    CHECK(h.bytes().undo > one.undo);
}

TEST_CASE("history budget: a step's cost follows what it OWNS, not sizeof") {
    World w;
    session::History h;
    h.set_enabled(true);
    voxel_step(h, w, 0, 10);
    const std::size_t small = h.bytes().undo;

    World w2;
    session::History h2;
    h2.set_enabled(true);
    voxel_step(h2, w2, 0, 5000);
    const std::size_t large = h2.bytes().undo;

    // Five hundred times the cells is not the same number of bytes, which is
    // exactly what sizeof would have reported.
    CHECK(large > small * 10);
}

TEST_CASE("history budget: removing an item costs more than adding one") {
    // The asymmetry the budget exists to make visible. The stack stores
    // INVERSES, so removing an item records a whole Node — 440 bytes plus its
    // deformer chain and stroke points — while adding one records an id.
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f)));
    const std::size_t after_add = h.bytes().undo;

    const scene::NodeId id = w.doc.layers[0].sdf->roots.back();
    scene::RemoveNodeCmd rm;
    rm.layer = w.sdf;
    rm.node = id;
    REQUIRE(h.perform(w.doc, scene::Command{rm}));
    const std::size_t after_remove = h.bytes().undo;

    // The remove's inverse carries the node; the add's carried an id.
    CHECK(after_remove - after_add > after_add / 2);
}

TEST_CASE("history budget: a budget evicts the oldest step") {
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 6; ++i) voxel_step(h, w, i * 1000, 200);
    const std::size_t depth_before = h.undo_depth();
    REQUIRE(depth_before == 6);

    const std::size_t half = h.bytes().undo / 2;
    h.set_budget(half);
    CHECK(h.undo_depth() < depth_before);
    CHECK(h.bytes().dropped_steps > 0);
}

TEST_CASE("history budget: the newest step survives any budget") {
    // A budget that could make the next undo fail would be worse than no
    // budget, because a host cannot tell that from a bug.
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 4; ++i) voxel_step(h, w, i * 1000, 500);

    h.set_budget(1);  // smaller than a single step
    CHECK(h.undo_depth() == 1);
    const std::size_t cells = w.grid.occupied_count();
    CHECK(h.undo(w.doc, w.grid_for(), w.mesh_for()));  // and it still works
    CHECK(w.grid.occupied_count() < cells);
}

TEST_CASE("history budget: redo is spent before undo") {
    // Redo is transient — the next edit discards it anyway — so spending the
    // budget on it before the undo the user can actually reach is the wrong
    // trade.
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 4; ++i) voxel_step(h, w, i * 1000, 300);
    for (int i = 0; i < 3; ++i) REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    REQUIRE(h.redo_depth() == 3);
    const std::size_t undo_depth = h.undo_depth();

    const session::History::Bytes b = h.bytes();
    h.set_budget(b.undo + b.redo / 2);
    CHECK(h.redo_depth() < 3);
    CHECK(h.undo_depth() == undo_depth);  // untouched
}

TEST_CASE("history budget: an unset budget is exactly today's behaviour") {
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 50; ++i) voxel_step(h, w, i * 100, 20);
    CHECK(h.undo_depth() == 50);
    CHECK(h.bytes().dropped_steps == 0);
    CHECK(h.budget() == 0);
}

TEST_CASE("history budget: trimming on demand does not need a budget") {
    // For a platform that reports memory pressure and expects an immediate
    // response rather than waiting for the next edit.
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 8; ++i) voxel_step(h, w, i * 1000, 200);
    const std::size_t before = h.bytes().undo;

    h.trim_to(before / 4);
    CHECK(h.bytes().undo < before);
    CHECK(h.undo_depth() >= 1);
    CHECK(h.budget() == 0);  // still unbounded; this was a one-off
}

TEST_CASE("history budget: truncation is observable, not an error") {
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 6; ++i) voxel_step(h, w, i * 1000, 200);
    h.set_budget(h.bytes().undo / 3);

    const session::History::Bytes b = h.bytes();
    CHECK(b.dropped_steps > 0);      // the horizon moved, and a host can say so
    CHECK(h.undo_depth() == b.undo_steps);
    // Still perfectly usable: undo works down to what is left.
    while (h.undo_depth() > 0) CHECK(h.undo(w.doc, w.grid_for(), w.mesh_for()));
}

TEST_CASE("history budget: a long session stays inside its budget") {
    // The regression for the defect itself, and the only test that would have
    // caught it: thousands of steps under a cap, asserting the history never
    // grows past it.
    World w;
    session::History h;
    h.set_enabled(true);
    constexpr std::size_t kBudget = 256 * 1024;
    h.set_budget(kBudget);

    for (int i = 0; i < 3000; ++i) {
        // Every write must CHANGE a cell, or the step is dropped and the
        // history never grows. Two earlier drafts of this got that wrong: the
        // first re-set cells to the value they already held, and the second
        // cycled 50 bases with a parity that made each base always receive the
        // SAME value — because 50 is even, so i and i+50 share a parity. The
        // value now flips each time the cycle comes round.
        REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
        const std::uint8_t value = ((i / 50) % 2) ? 1 : 0;
        for (int c = 0; c < 40; ++c) w.grid.set({(i % 50) * 100 + c, 0, 0}, value);
        h.end_voxel_step(w.grid);
        if (i % 200 == 0) {
            const session::History::Bytes b = h.bytes();
            CHECK(b.undo + b.redo <= kBudget);
        }
    }
    const session::History::Bytes end = h.bytes();
    CHECK(end.undo + end.redo <= kBudget);
    CHECK(end.dropped_steps > 0);
    CHECK(h.undo_depth() >= 1);
}

TEST_CASE("history budget: the journal is reported but NOT evicted") {
    // Those bytes are the host's crash recovery. Dropping them silently would
    // lose exactly what the feature exists to keep, so the budget reports the
    // journal and the host trims it once its bytes are durable.
    World w;
    session::History h;
    h.set_enabled(true);
    for (int i = 0; i < 10; ++i) voxel_step(h, w, i * 1000, 200);
    const std::size_t journal_before = h.bytes().journal;
    REQUIRE(journal_before > 0);

    h.set_budget(1);
    CHECK(h.bytes().journal == journal_before);  // untouched by the budget

    // The host's own lever still works.
    h.trim_journal(h.journal_next());
    CHECK(h.bytes().journal == 0);
}

// -- a group is one step across representations (#341) ------------------------
//
// begin_group forwarded to UndoStack::begin_group and nothing else, so it
// grouped SCENE COMMANDS and only those. A voxel, mask or mesh step recorded
// between the brackets was pushed straight onto the step list and stayed its
// own undo — which made a crossing (create a voxel layer, rasterize into it)
// two steps, the first of which removed the layer out from under the fill it
// contained.

TEST_CASE("history group: a bracket is one step, whatever it spans") {
    World w;
    session::History h;
    h.set_enabled(true);

    h.begin_group();
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f)));
    voxel_step(h, w, 0, 8);
    h.end_group();

    CHECK(h.undo_depth() == 1);  // not 2
    CHECK(h.bytes().undo_steps == 1);
    // The fold owns its children, so the cost is theirs — otherwise a budget
    // would read a collapsed group as an empty Step and never evict it.
    CHECK(h.bytes().undo > sizeof(session::Step) * 2);

    const std::size_t filled = w.grid.occupied_count();
    CHECK(filled == 8);
    const std::size_t nodes = w.doc.layers[0].sdf->roots.size();

    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(h.undo_depth() == 0);
    CHECK(w.grid.occupied_count() == 0);                    // the cells went
    CHECK(w.doc.layers[0].sdf->roots.size() == nodes - 1);  // and so did the sphere

    REQUIRE(h.redo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(h.undo_depth() == 1);
    CHECK(w.grid.occupied_count() == filled);
    CHECK(w.doc.layers[0].sdf->roots.size() == nodes);
}

TEST_CASE("history group: a bracket of commands alone is unchanged") {
    // The fold must not wrap a lone scene entry, or a host that has always
    // bracketed its command bursts sees a different step shape for no reason.
    World w;
    session::History h;
    h.set_enabled(true);

    h.begin_group();
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f)));
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.25f)));
    h.end_group();

    CHECK(h.undo_depth() == 1);
    CHECK(h.step_count() == 1);
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(w.doc.layers[0].sdf->roots.empty());  // both, as one step
}

TEST_CASE("history group: a barrier inside a bracket is not swallowed") {
    // A barrier is the horizon a host draws. Folded into a reversible compound
    // it would offer an undo straight across the thing that says you cannot go
    // back, so a bracket holding one is left alone.
    World w;
    session::History h;
    h.set_enabled(true);

    h.begin_group();
    voxel_step(h, w, 0, 4);
    h.record_barrier("dropped a resolution level");
    h.end_group();

    CHECK(h.undo_depth() == 0);  // the barrier is on top and stops the count
    CHECK(h.next_barrier() == "dropped a resolution level");
    CHECK(h.step_count() == 2);  // still two steps, not one folded one
    CHECK_FALSE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
}

TEST_CASE("history group: an empty bracket still records nothing") {
    World w;
    session::History h;
    h.set_enabled(true);
    h.begin_group();
    h.end_group();
    CHECK(h.step_count() == 0);
    CHECK(h.undo_depth() == 0);
}

TEST_CASE("history group: a refused child leaves the whole step unapplied") {
    // Half a compound is exactly the state this exists to remove. The voxel
    // child names a layer the resolver cannot find, so the step must refuse
    // and put back the scene child it had already reversed.
    World w;
    session::History h;
    h.set_enabled(true);

    h.begin_group();
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f)));
    voxel_step(h, w, 0, 4);
    h.end_group();
    const std::size_t nodes = w.doc.layers[0].sdf->roots.size();

    // A resolver that knows no layer at all.
    session::History::GridFor none = [](scene::LayerId) -> voxel::VoxelGrid* { return nullptr; };
    CHECK_FALSE(h.undo(w.doc, none, w.mesh_for()));
    CHECK(h.undo_depth() == 1);                            // still on the stack
    CHECK(w.doc.layers[0].sdf->roots.size() == nodes);     // and the sphere is back
    CHECK(w.grid.occupied_count() == 4);                   // nothing moved

    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));    // with a real resolver it goes
    CHECK(h.undo_depth() == 0);
}
