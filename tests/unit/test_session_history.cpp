#include <doctest/doctest.h>

#include <vector>

#include "clay/mesh/sculpt.h"
#include "clay/scene/commands.h"
#include "clay/session/history.h"
#include "clay/voxel/grid.h"

// One undo across three representations (scene-model spec: one undo order
// spans every representation; the history says what it cannot reverse).
//
// The regression this whole change is for: an SDF stamp, a voxel smooth and a
// mesh grab undo in the reverse of the order they were made. Before this, the
// three mechanisms were unordered with respect to each other and
// clay_document_undo reached only the first of them.

using namespace clay;

namespace {

scene::Command add_sphere(scene::LayerId layer, float r, kernel::cfloat3 at) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    n.xform.position = at;
    scene::AddNodeCmd cmd;
    cmd.layer = layer;
    cmd.subtree.push_back(n);
    return scene::Command{cmd};
}

// The resolvers the history takes, because the object that owns the three
// representations sits above its module.
struct World {
    scene::Document doc;
    scene::LayerId sdf_layer = 0;
    voxel::VoxelGrid grid{0.1f};
    mesh::Mesh mesh;
    scene::LayerId voxel_layer = 7;  // ids the resolvers answer for
    scene::LayerId mesh_layer = 9;

    World() {
        scene::Layer& l = doc.add_sdf_layer("body");
        sdf_layer = l.id;
        // A little geometry for the mesh side: two triangles is enough to move.
        mesh.positions = {kernel::cf3(0, 0, 0), kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0),
                          kernel::cf3(1, 1, 0)};
        mesh.indices = {0, 1, 2, 1, 3, 2};
    }

    session::History::GridFor grid_for() {
        return [this](scene::LayerId id) -> voxel::VoxelGrid* {
            return id == voxel_layer ? &grid : nullptr;
        };
    }
    session::History::MeshFor mesh_for() {
        return [this](scene::LayerId id) -> mesh::Mesh* {
            return id == mesh_layer ? &mesh : nullptr;
        };
    }
};

// A mesh step, as a sculptor would produce it: move one vertex and record what
// it was.
mesh::VertexDeltas move_vertex(mesh::Mesh& m, std::uint32_t v, kernel::cfloat3 to) {
    mesh::VertexDeltas deltas;
    deltas.note(v, m);       // where it was
    m.positions[v] = to;
    deltas.sync_after(v, m);  // where it ended up
    return deltas;
}

}  // namespace

TEST_CASE("session: one undo order spans SDF, voxel and mesh") {
    World w;
    session::History h;
    h.set_enabled(true);

    // 1. an SDF item
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    const std::size_t nodes_after_add = w.doc.layers[0].sdf->roots.size();

    // 2. a voxel edit
    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    w.grid.set({1, 0, 0}, 1);
    h.end_voxel_step(w.grid);
    REQUIRE(w.grid.occupied_count() == 2);

    // 3. a mesh edit
    const kernel::cfloat3 before = w.mesh.positions[0];
    h.record_mesh_step(w.mesh_layer, move_vertex(w.mesh, 0, kernel::cf3(0, 0, 1)));
    REQUIRE(w.mesh.positions[0].z == doctest::Approx(1.0f));

    CHECK(h.undo_depth() == 3);

    // Reverse order: mesh, then voxel, then SDF.
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(w.mesh.positions[0].z == doctest::Approx(before.z));
    CHECK(w.grid.occupied_count() == 2);  // untouched by the mesh undo

    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(w.grid.occupied_count() == 0);

    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(w.doc.layers[0].sdf->roots.size() == nodes_after_add - 1);

    CHECK(h.undo_depth() == 0);
    CHECK(h.redo_depth() == 3);
    CHECK_FALSE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
}

TEST_CASE("session: redo restores the same order") {
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({2, 2, 2}, 1);
    h.end_voxel_step(w.grid);
    h.record_mesh_step(w.mesh_layer, move_vertex(w.mesh, 1, kernel::cf3(5, 0, 0)));

    const std::size_t nodes = w.doc.layers[0].sdf->roots.size();
    const std::size_t cells = w.grid.occupied_count();
    const kernel::cfloat3 moved = w.mesh.positions[1];

    for (int i = 0; i < 3; ++i) REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    for (int i = 0; i < 3; ++i) REQUIRE(h.redo(w.doc, w.grid_for(), w.mesh_for()));

    CHECK(w.doc.layers[0].sdf->roots.size() == nodes);
    CHECK(w.grid.occupied_count() == cells);
    CHECK(w.mesh.positions[1].x == doctest::Approx(moved.x));
    CHECK(h.undo_depth() == 3);
    CHECK(h.redo_depth() == 0);
}

TEST_CASE("session: a voxel step that changed nothing is not a step") {
    // A dab that misses every cell is ordinary here, and an undo that does
    // nothing is exactly what this change exists to remove.
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    h.end_voxel_step(w.grid);
    CHECK(h.step_count() == 0);
    CHECK(h.undo_depth() == 0);

    // And a write that sets a cell to what it ALREADY HELD records nothing, so
    // it is not a step either. This assertion said the opposite when first
    // written, and the C-level test caught it: journaling a write that changed
    // nothing builds an undo step that undoes nothing, which is the exact
    // defect this channel exists to avoid.
    w.grid.set({0, 0, 0}, 1);
    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    h.end_voxel_step(w.grid);
    CHECK(h.step_count() == 0);
}

TEST_CASE("session: a stroke that coalesces is still one step") {
    // Coalescing lives in UndoStack and must keep working: the session pushes a
    // step only when the command stack actually grew.
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    const scene::NodeId id = w.doc.layers[0].sdf->roots.back();
    const std::size_t after_add = h.undo_depth();

    for (int i = 0; i < 8; ++i) {
        scene::AppendStrokeCmd cmd;
        cmd.layer = w.sdf_layer;
        cmd.node = id;
        cmd.points.push_back(scene::StrokePoint{kernel::cf3(float(i) * 0.1f, 0, 0), 0.05f});
        h.perform(w.doc, scene::Command{cmd});
    }

    // Eight appends on one node coalesce into one entry, so at most one step.
    CHECK(h.undo_depth() <= after_add + 1);
}

TEST_CASE("session: a barrier is a horizon, not a silent gap") {
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    h.record_barrier("mask edit");
    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    h.end_voxel_step(w.grid);

    // Three recorded, but only the one above the barrier can be undone — a
    // depth that counted further would promise an undo the host cannot do.
    CHECK(h.step_count() == 3);
    CHECK(h.undo_depth() == 1);
    CHECK(h.next_barrier() == "mask edit");

    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(h.undo_depth() == 0);
    // The barrier is not consumed: it stays as the horizon.
    CHECK_FALSE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(h.step_count() == 2);
}

TEST_CASE("session: a new edit discards redo across representations") {
    // Keeping redo would let a redo replay a voxel run onto cells a later SDF
    // edit has already moved.
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    h.end_voxel_step(w.grid);
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(h.redo_depth() == 1);

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    CHECK(h.redo_depth() == 0);
}

TEST_CASE("session: a step whose layer is gone is refused, not skipped") {
    // Skipping would take the step off the stack and leave the user's next
    // undo reversing something older than they asked for.
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    h.end_voxel_step(w.grid);

    auto no_grid = [](scene::LayerId) -> voxel::VoxelGrid* { return nullptr; };
    CHECK_FALSE(h.undo(w.doc, no_grid, w.mesh_for()));
    CHECK(h.undo_depth() == 1);  // still there
    CHECK(w.grid.occupied_count() == 1);

    // And with the grid back, it undoes.
    CHECK(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(w.grid.occupied_count() == 0);
}

TEST_CASE("session: disabled records nothing and behaves as before") {
    World w;
    session::History h;  // not enabled

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    CHECK(w.doc.layers[0].sdf->roots.size() == 1);  // the edit still happened
    CHECK(h.step_count() == 0);
    CHECK_FALSE(h.begin_voxel_step(w.voxel_layer, w.grid));
    CHECK(w.grid.change_sink() == nullptr);
    h.record_barrier("mask edit");
    CHECK(h.step_count() == 0);
    CHECK_FALSE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
}

TEST_CASE("session: a nested voxel step is refused") {
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    CHECK_FALSE(h.begin_voxel_step(w.voxel_layer, w.grid));
    h.end_voxel_step(w.grid);
    CHECK(w.grid.change_sink() == nullptr);
}

TEST_CASE("voxel: the sink records every write in order, and does not coalesce") {
    // The sculpt-layer channel coalesces by cell because a strength dial
    // re-picks a pass's NET effect. A replay must unwind writes in the order
    // they were made, so this channel deliberately does not.
    voxel::VoxelGrid grid(0.1f);
    std::vector<voxel::VoxelGrid::SculptChange> sink;
    grid.set_change_sink(&sink);
    grid.set({0, 0, 0}, 1);
    grid.set({0, 0, 0}, 2);
    grid.set({0, 0, 0}, 2);  // changes nothing: not journaled
    grid.set({1, 0, 0}, 3);
    grid.set_change_sink(nullptr);

    REQUIRE(sink.size() == 3);
    CHECK(sink[0].before == 0);
    CHECK(sink[0].after == 1);
    CHECK(sink[1].before == 1);  // the second write to the same cell
    CHECK(sink[1].after == 2);

    // Reverting unwinds to the starting state, twice-written cell included.
    grid.revert_changes(sink);
    CHECK(grid.occupied_count() == 0);
    grid.reapply_changes(sink);
    CHECK(grid.get({0, 0, 0}) == 2);
    CHECK(grid.get({1, 0, 0}) == 3);
}

TEST_CASE("voxel: a replay is not a pass and records into neither channel") {
    voxel::VoxelGrid grid(0.1f);
    std::vector<voxel::VoxelGrid::SculptChange> sink;
    grid.set_change_sink(&sink);
    grid.set({0, 0, 0}, 1);
    const std::vector<voxel::VoxelGrid::SculptChange> recorded = sink;

    // A revert while the sink is still installed must not journal itself, or
    // an undo would grow the thing it is unwinding.
    grid.revert_changes(recorded);
    CHECK(sink.size() == recorded.size());
    grid.set_change_sink(nullptr);
}

TEST_CASE("session: a group is one step, and an empty group is none") {
    // Regression. UndoStack::begin_group pushes its entry IMMEDIATELY, so the
    // stack's depth grows at BEGIN and the commands inside append without
    // growing it further. The first draft detected steps with "did the depth
    // grow across this call", which is true for an ordinary command and false
    // for every part of a group — so a grouped edit recorded no step at all and
    // four existing tests went red. The reconciliation at end_group is the fix.
    World w;
    session::History h;
    h.set_enabled(true);

    h.begin_group();
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.3f, kernel::cf3(1, 0, 0))));
    h.end_group();

    CHECK(h.undo_depth() == 1);  // two commands, one step
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    CHECK(w.doc.layers[0].sdf->roots.empty());  // both came back

    // A group nothing was done in is not a step.
    const std::size_t before = h.step_count();
    h.begin_group();
    h.end_group();
    CHECK(h.step_count() == before);
}

TEST_CASE("session: a group interleaves with the other representations") {
    World w;
    session::History h;
    h.set_enabled(true);

    h.begin_group();
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.5f, kernel::cf3(0, 0, 0))));
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf_layer, 0.3f, kernel::cf3(1, 0, 0))));
    h.end_group();

    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    h.end_voxel_step(w.grid);

    CHECK(h.undo_depth() == 2);
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));  // the voxel step
    CHECK(w.grid.occupied_count() == 0);
    CHECK(w.doc.layers[0].sdf->roots.size() == 2);       // the group is intact
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));  // the whole group
    CHECK(w.doc.layers[0].sdf->roots.empty());
}
