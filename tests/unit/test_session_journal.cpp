#include <doctest/doctest.h>

#include <vector>

#include "clay/mesh/sculpt.h"
#include "clay/scene/commands.h"
#include "clay/session/history.h"
#include "clay/voxel/grid.h"

// Crash recovery: a snapshot plus the steps since it (survive-a-crash).
//
// The test this change exists for is the LAST one here: a session edited across
// all three representations, journaled, and replayed onto a fresh world that
// then matches. A journal that carried two of the three kinds would recover two
// thirds of a session and say nothing about the missing third, which is the
// failure this whole feature is ordered to avoid.

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

struct World {
    scene::Document doc;
    scene::LayerId sdf = 0;
    voxel::VoxelGrid grid{0.1f};
    mesh::Mesh mesh;
    scene::LayerId voxel_layer = 7;
    scene::LayerId mesh_layer = 9;

    World() {
        sdf = doc.add_sdf_layer("body").id;
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

mesh::VertexDeltas move_vertex(mesh::Mesh& m, std::uint32_t v, kernel::cfloat3 to) {
    mesh::VertexDeltas d;
    d.note(v, m);
    m.positions[v] = to;
    d.sync_after(v, m);
    return d;
}

}  // namespace

TEST_CASE("journal: vertex deltas round-trip through their encoding") {
    // The one genuinely new serializer this change needed: an edit-list step is
    // a scene::Command, which the document format already encodes, and a voxel
    // step is a run of PODs. This was the third.
    mesh::Mesh m;
    m.positions = {kernel::cf3(0, 0, 0), kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0)};
    m.normals = {kernel::cf3(0, 0, 1), kernel::cf3(0, 0, 1), kernel::cf3(0, 0, 1)};
    m.colors = {kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0), kernel::cf3(0, 0, 1)};
    m.indices = {0, 1, 2};

    const mesh::Mesh original = m;
    mesh::VertexDeltas d = move_vertex(m, 1, kernel::cf3(5, 5, 5));
    const std::vector<std::uint8_t> bytes = d.encode();
    REQUIRE(!bytes.empty());

    mesh::VertexDeltas back;
    REQUIRE(mesh::VertexDeltas::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.size() == d.size());
    CHECK(back.vertices() == d.vertices());

    // The decoded record reverts a mesh exactly as the original did.
    mesh::Mesh a = m, b = m;
    CHECK(d.revert(a));
    CHECK(back.revert(b));
    CHECK(a.positions[1].x == doctest::Approx(b.positions[1].x));
    CHECK(b.positions[1].x == doctest::Approx(original.positions[1].x));
    CHECK(b.indices == original.indices);  // untouched by either
}

TEST_CASE("journal: a malformed vertex-delta record is refused") {
    mesh::Mesh m;
    m.positions = {kernel::cf3(0, 0, 0), kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0)};
    m.indices = {0, 1, 2};
    mesh::VertexDeltas d = move_vertex(m, 1, kernel::cf3(5, 5, 5));
    const std::vector<std::uint8_t> bytes = d.encode();

    mesh::VertexDeltas out;
    CHECK_FALSE(mesh::VertexDeltas::decode(nullptr, 10, &out));
    CHECK_FALSE(mesh::VertexDeltas::decode(bytes.data(), 3, &out));  // shorter than the magic
    // Every truncation is refused rather than producing a record that would
    // revert a mesh to values that were never in it.
    for (std::size_t cut = 4; cut < bytes.size(); cut += 5)
        CHECK_FALSE(mesh::VertexDeltas::decode(bytes.data(), cut, &out));

    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] ^= 0xFF;  // magic
    CHECK_FALSE(mesh::VertexDeltas::decode(wrong.data(), wrong.size(), &out));
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 99;  // version
    CHECK_FALSE(mesh::VertexDeltas::decode(newer.data(), newer.size(), &out));
}

TEST_CASE("journal: an unreadable journal is refused, not partly applied") {
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);

    World fresh;
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;

    CHECK_FALSE(replayer.replay(nullptr, 0, fresh.doc, fresh.grid_for(), fresh.mesh_for(), &res));
    std::vector<std::uint8_t> wrong = journal;
    wrong[0] ^= 0xFF;
    CHECK_FALSE(replayer.replay(wrong.data(), wrong.size(), fresh.doc, fresh.grid_for(),
                                fresh.mesh_for(), &res));
    std::vector<std::uint8_t> newer = journal;
    newer[4] = 99;
    CHECK_FALSE(replayer.replay(newer.data(), newer.size(), fresh.doc, fresh.grid_for(),
                                fresh.mesh_for(), &res));
    CHECK(fresh.doc.layers[0].sdf->roots.empty());  // nothing applied
}

TEST_CASE("journal: replay stops at a barrier rather than skipping it") {
    // A recovery that silently skipped would hand back a document quietly
    // missing that operation's effect, and the user could not see the loss.
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    h.record_barrier("dropped a resolution level");
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.3f, kernel::cf3(1, 0, 0))));

    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);

    World fresh;
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(journal.data(), journal.size(), fresh.doc, fresh.grid_for(),
                            fresh.mesh_for(), &res));
    CHECK(res.stopped_at_barrier);
    CHECK(res.barrier == "dropped a resolution level");
    // The item before the barrier is there; the one after is not.
    CHECK(fresh.doc.layers[0].sdf->roots.size() == 1);
}

TEST_CASE("journal: an undo is an event, so replay does not restore what was taken back") {
    // The correctness argument for an append-only LOG rather than a view of the
    // step list: the host persisted the step, then the user undid it. A journal
    // read off the step list would no longer contain it, but the host's file
    // still would.
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.3f, kernel::cf3(1, 0, 0))));
    REQUIRE(h.undo(w.doc, w.grid_for(), w.mesh_for()));
    REQUIRE(w.doc.layers[0].sdf->roots.size() == 1);

    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);

    World fresh;
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(journal.data(), journal.size(), fresh.doc, fresh.grid_for(),
                            fresh.mesh_for(), &res));
    CHECK(fresh.doc.layers[0].sdf->roots.size() == 1);  // the undo was reproduced
}

TEST_CASE("journal: it is incremental, and trimming moves the floor") {
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    std::size_t first_at = 0;
    const std::vector<std::uint8_t> a = h.journal_since(0, &first_at);
    CHECK(first_at == 1);

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.3f, kernel::cf3(1, 0, 0))));
    std::size_t second_at = 0;
    const std::vector<std::uint8_t> b = h.journal_since(first_at, &second_at);
    CHECK(second_at == 2);
    CHECK(b.size() < a.size() + 64);  // only the new event, not the whole log

    World fresh;
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(a.data(), a.size(), fresh.doc, fresh.grid_for(), fresh.mesh_for(),
                            &res));
    REQUIRE(replayer.replay(b.data(), b.size(), fresh.doc, fresh.grid_for(), fresh.mesh_for(),
                            &res));
    CHECK(fresh.doc.layers[0].sdf->roots.size() == 2);

    // Trimming what is durable moves the floor; asking below it yields nothing
    // rather than a silently shorter history.
    h.trim_journal(first_at);
    CHECK(h.journal_first() == first_at);
    std::size_t ignored = 0;
    const std::vector<std::uint8_t> stale = h.journal_since(0, &ignored);
    World empty_world;
    session::History empty_replayer;
    empty_replayer.set_enabled(true);
    session::History::ReplayResult none;
    REQUIRE(empty_replayer.replay(stale.data(), stale.size(), empty_world.doc,
                                  empty_world.grid_for(), empty_world.mesh_for(), &none));
    CHECK(none.applied == 0);
}

TEST_CASE("journal: a session across ALL THREE representations is reconstructed") {
    // The test this change exists for.
    World w;
    session::History h;
    h.set_enabled(true);

    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));

    REQUIRE(h.begin_voxel_step(w.voxel_layer, w.grid));
    w.grid.set({0, 0, 0}, 1);
    w.grid.set({1, 0, 0}, 2);
    w.grid.set({0, 1, 0}, 1);
    h.end_voxel_step(w.grid);

    h.record_mesh_step(w.mesh_layer, move_vertex(w.mesh, 0, kernel::cf3(0, 0, 3)));

    // A group, so coalescing and grouping have to reproduce themselves.
    h.begin_group();
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.2f, kernel::cf3(2, 0, 0))));
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.2f, kernel::cf3(3, 0, 0))));
    h.end_group();

    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);
    REQUIRE(!journal.empty());

    // A fresh world standing in for "the snapshot, reloaded after the crash".
    World fresh;
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(journal.data(), journal.size(), fresh.doc, fresh.grid_for(),
                            fresh.mesh_for(), &res));
    CHECK_FALSE(res.stopped_at_barrier);

    // SDF: the same nodes.
    CHECK(fresh.doc.layers[0].sdf->roots.size() == w.doc.layers[0].sdf->roots.size());
    // Voxels: the same cells, with the same palette indices.
    CHECK(fresh.grid.occupied_count() == w.grid.occupied_count());
    CHECK(fresh.grid.get({0, 0, 0}) == w.grid.get({0, 0, 0}));
    CHECK(fresh.grid.get({1, 0, 0}) == w.grid.get({1, 0, 0}));
    CHECK(fresh.grid.get({0, 1, 0}) == w.grid.get({0, 1, 0}));
    // Mesh: the same vertex, moved.
    CHECK(fresh.mesh.positions[0].z == doctest::Approx(w.mesh.positions[0].z));

    // And the reconstructed session is still undoable, because replay went
    // through the same recording path the original session did.
    CHECK(replayer.undo_depth() == h.undo_depth());
}
