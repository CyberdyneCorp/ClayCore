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
        mesh.normals = {kernel::cf3(0, 0, 1), kernel::cf3(0, 0, 1), kernel::cf3(0, 0, 1),
                        kernel::cf3(0, 0, 1)};
        mesh.colors = {kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0), kernel::cf3(0, 0, 1),
                       kernel::cf3(1, 1, 0)};
        mesh.indices = {0, 1, 2, 1, 3, 2};
        mesh.quads = {0, 1, 3, 2};
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

mesh::VertexDeltas repaint_vertex(mesh::Mesh& m, std::uint32_t v, kernel::cfloat3 to,
                                  kernel::cfloat3 normal, kernel::cfloat3 color) {
    mesh::VertexDeltas d;
    d.note(v, m);
    m.positions[v] = to;
    m.normals[v] = normal;
    m.colors[v] = color;
    d.sync_after(v, m);
    return d;
}

}  // namespace

TEST_CASE("journal: vertex deltas round-trip through their encoding") {
    // The one genuinely new serializer this change needed: an edit-list step is
    // a scene::Command, which the document format already encodes, and a voxel
    // step is a run of PODs. This was the third.
    mesh::Mesh m;
    m.positions = {kernel::cf3(0, 0, 0), kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0),
                   kernel::cf3(1, 1, 0)};
    m.normals = {kernel::cf3(0, 0, 1), kernel::cf3(0, 0, 1), kernel::cf3(0, 0, 1),
                 kernel::cf3(0, 0, 1)};
    m.colors = {kernel::cf3(1, 0, 0), kernel::cf3(0, 1, 0), kernel::cf3(0, 0, 1),
                kernel::cf3(1, 1, 0)};
    m.indices = {0, 1, 2, 1, 3, 2};
    // A quad mesh, because the record promises to leave BOTH index arrays
    // alone and a triangle-only fixture could only ever prove half of that.
    m.quads = {0, 1, 3, 2};

    const mesh::Mesh original = m;
    mesh::VertexDeltas d =
        repaint_vertex(m, 1, kernel::cf3(5, 5, 5), kernel::cf3(0, 1, 0), kernel::cf3(0.25f, 0.5f,
                                                                                     0.75f));
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
    // The normal and the colour come back too, and byte for byte: an imported
    // model's attributes are whatever its author wrote, and a recovery that
    // recomputed them would return a mesh that is geometrically identical and
    // not the one the artist had.
    CHECK(b.normals[1].y == doctest::Approx(original.normals[1].y));
    CHECK(b.colors[1].x == doctest::Approx(original.colors[1].x));
    CHECK(b.colors[1].z == doctest::Approx(original.colors[1].z));
    CHECK(b.indices == original.indices);  // untouched by either
    CHECK(b.quads == original.quads);

    // And re-applying the decoded record puts the edit back, still without
    // touching either index array.
    CHECK(back.apply(b));
    CHECK(b.positions[1].x == doctest::Approx(5.0f));
    CHECK(b.normals[1].y == doctest::Approx(1.0f));
    CHECK(b.colors[1].y == doctest::Approx(0.5f));
    CHECK(b.indices == original.indices);
    CHECK(b.quads == original.quads);
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

TEST_CASE("journal: a replayed mesh step brings back normals and colours, not just positions") {
    // The file-io delta says "each kind is reconstructed, and the mesh layer's
    // vertices, normals and colours match what they were". The all-three case
    // below moves a vertex; this one repaints it, so the two attributes a
    // position-only assertion cannot see have something to prove.
    World w;
    session::History h;
    h.set_enabled(true);
    h.record_mesh_step(w.mesh_layer, repaint_vertex(w.mesh, 2, kernel::cf3(0, 1, 4),
                                                    kernel::cf3(0, 1, 0),
                                                    kernel::cf3(0.125f, 0.25f, 0.5f)));

    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);

    World fresh;
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(journal.data(), journal.size(), fresh.doc, fresh.grid_for(),
                            fresh.mesh_for(), &res));
    CHECK(res.applied == 1);
    CHECK(fresh.mesh.positions[2].z == doctest::Approx(w.mesh.positions[2].z));
    CHECK(fresh.mesh.normals[2].y == doctest::Approx(w.mesh.normals[2].y));
    CHECK(fresh.mesh.colors[2].x == doctest::Approx(w.mesh.colors[2].x));
    CHECK(fresh.mesh.colors[2].z == doctest::Approx(w.mesh.colors[2].z));
    // Neither index array is written by a replay, for the same reason neither
    // is recorded: the fixed-topology contract.
    CHECK(fresh.mesh.indices == w.mesh.indices);
    CHECK(fresh.mesh.quads == w.mesh.quads);
}

TEST_CASE("journal: the pair is checked, and a journal from another snapshot is refused") {
    // 2.1. Without this a journal replayed onto the wrong snapshot applies
    // cleanly and hands back a document matching neither: the edit list holds
    // what the snapshot already had, twice over, and nothing says so.
    World w;
    session::History h;
    h.set_enabled(true);
    h.note_snapshot(0xA1A1A1A1ull);  // the host wrote a snapshot here
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));

    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);

    SUBCASE("onto the snapshot it names, it replays") {
        World right;
        right.doc.snapshot_id = 0xA1A1A1A1ull;
        session::History replayer;
        replayer.set_enabled(true);
        session::History::ReplayResult res;
        REQUIRE(replayer.replay(journal.data(), journal.size(), right.doc, right.grid_for(),
                                right.mesh_for(), &res));
        CHECK(res.applied == 1);
        CHECK_FALSE(res.snapshot_mismatch);
    }

    SUBCASE("onto a different snapshot it is refused, with nothing applied") {
        World other;
        other.doc.snapshot_id = 0xB2B2B2B2ull;
        session::History replayer;
        replayer.set_enabled(true);
        session::History::ReplayResult res;
        CHECK_FALSE(replayer.replay(journal.data(), journal.size(), other.doc, other.grid_for(),
                                    other.mesh_for(), &res));
        CHECK(res.snapshot_mismatch);
        CHECK(res.journal_snapshot_id == 0xA1A1A1A1ull);
        CHECK(res.applied == 0);
        // The identity is in the HEADER, so this is the one refusal replay has
        // that leaves the document exactly as it found it.
        CHECK(other.doc.layers[0].sdf->roots.empty());
    }

    SUBCASE("onto a document that was never serialized it is refused too") {
        // A document built from scratch cannot be the recovery it looks like.
        World scratch;  // snapshot_id stays 0
        session::History replayer;
        replayer.set_enabled(true);
        session::History::ReplayResult res;
        CHECK_FALSE(replayer.replay(journal.data(), journal.size(), scratch.doc,
                                    scratch.grid_for(), scratch.mesh_for(), &res));
        CHECK(res.snapshot_mismatch);
        CHECK(scratch.doc.layers[0].sdf->roots.empty());
    }
}

TEST_CASE("journal: a journal that names no snapshot is replayed rather than refused") {
    // ONE-DIRECTIONAL on purpose. A session that never serialized makes no
    // claim, and so does every journal written before the identity existed —
    // refusing those would have turned an upgrade into the data loss this
    // whole feature is here to prevent.
    World w;
    session::History h;
    h.set_enabled(true);  // no note_snapshot: nothing was ever saved
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    std::size_t at = 0;
    const std::vector<std::uint8_t> journal = h.journal_since(0, &at);

    World loaded;
    loaded.doc.snapshot_id = 0xC3C3C3C3ull;  // this one WAS loaded from bytes
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(journal.data(), journal.size(), loaded.doc, loaded.grid_for(),
                            loaded.mesh_for(), &res));
    CHECK(res.applied == 1);
    CHECK_FALSE(res.snapshot_mismatch);

    SUBCASE("and so is a version 1 journal, which is the format before the identity") {
        // Version 1's header is the same minus the eight identity bytes. A
        // build that refused it would refuse exactly the recovery file a user
        // is holding when they upgrade after a crash.
        World named;
        session::History h2;
        h2.set_enabled(true);
        h2.note_snapshot(0xD4D4D4D4ull);
        REQUIRE(h2.perform(named.doc, add_sphere(named.sdf, 0.5f, kernel::cf3(0, 0, 0))));
        std::size_t at2 = 0;
        std::vector<std::uint8_t> v1 = h2.journal_since(0, &at2);
        REQUIRE(v1.size() > 16);
        v1[4] = 1;  // version
        v1.erase(v1.begin() + 8, v1.begin() + 16);  // the identity field

        World into;
        into.doc.snapshot_id = 0xE5E5E5E5ull;  // a different snapshot entirely
        session::History old_replayer;
        old_replayer.set_enabled(true);
        session::History::ReplayResult old_res;
        REQUIRE(old_replayer.replay(v1.data(), v1.size(), into.doc, into.grid_for(),
                                    into.mesh_for(), &old_res));
        CHECK(old_res.applied == 1);
        CHECK(old_res.journal_snapshot_id == 0);
    }
}

TEST_CASE("journal: the snapshot it names is the one CURRENT at the index, not the newest") {
    // The reason the identity is keyed on the journal index. A host snapshots,
    // keeps journaling, and serializes AGAIN — to compare the journal against
    // the document, which is the re-snapshot rule this feature documents.
    // Stamped with "the last thing serialized", the events before that second
    // image would be refused against the snapshot the host kept, and ACCEPTED
    // against the second image, where they apply a second time.
    World w;
    session::History h;
    h.set_enabled(true);
    h.note_snapshot(0x1111ull);  // the snapshot the host keeps, at index 0
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.4f, kernel::cf3(1, 0, 0))));
    const std::size_t at_second = h.journal_next();
    h.note_snapshot(0x2222ull);  // serialized again, and discarded
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.3f, kernel::cf3(2, 0, 0))));

    std::size_t at = 0;
    const std::vector<std::uint8_t> from_start = h.journal_since(0, &at);
    std::size_t at2 = 0;
    const std::vector<std::uint8_t> from_second = h.journal_since(at_second, &at2);

    // The early segment still names the snapshot that was current when those
    // events happened...
    World kept;
    kept.doc.snapshot_id = 0x1111ull;
    session::History r1;
    r1.set_enabled(true);
    session::History::ReplayResult res1;
    REQUIRE(r1.replay(from_start.data(), from_start.size(), kept.doc, kept.grid_for(),
                      kept.mesh_for(), &res1));
    CHECK(res1.applied == 3);

    // ...and is refused against the LATER image, which already contains them.
    World later;
    later.doc.snapshot_id = 0x2222ull;
    session::History r2;
    r2.set_enabled(true);
    session::History::ReplayResult res2;
    CHECK_FALSE(r2.replay(from_start.data(), from_start.size(), later.doc, later.grid_for(),
                          later.mesh_for(), &res2));
    CHECK(res2.snapshot_mismatch);
    CHECK(res2.journal_snapshot_id == 0x1111ull);
    CHECK(later.doc.layers[0].sdf->roots.empty());

    // The segment taken from the second save names the second image, and
    // replays onto it.
    World later2;
    later2.doc.snapshot_id = 0x2222ull;
    session::History r3;
    r3.set_enabled(true);
    session::History::ReplayResult res3;
    REQUIRE(r3.replay(from_second.data(), from_second.size(), later2.doc, later2.grid_for(),
                      later2.mesh_for(), &res3));
    CHECK(res3.applied == 1);
    CHECK(res3.journal_snapshot_id == 0x2222ull);

    // A trim keeps the entry a lookup at the floor still needs, rather than
    // dropping the identity along with the events.
    h.trim_journal(at_second);
    std::size_t at3 = 0;
    const std::vector<std::uint8_t> after_trim = h.journal_since(at_second, &at3);
    World later3;
    later3.doc.snapshot_id = 0x2222ull;
    session::History r4;
    r4.set_enabled(true);
    session::History::ReplayResult res4;
    REQUIRE(r4.replay(after_trim.data(), after_trim.size(), later3.doc, later3.grid_for(),
                      later3.mesh_for(), &res4));
    CHECK(res4.journal_snapshot_id == 0x2222ull);
}

TEST_CASE("journal: a barrier is visible in the log before a recovery needs it") {
    // Rule 1 of the two the design states, and the half that had no mechanism:
    // replay reports a barrier, but replay happens during the recovery — the
    // one moment when "you needed a fresher snapshot" is useless, because the
    // session that would have been snapshotted is gone.
    World w;
    session::History h;
    h.set_enabled(true);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));

    std::size_t at = 0;
    CHECK_FALSE(h.journal_barrier_after(0, &at));

    h.record_barrier("dropped a resolution level");
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.3f, kernel::cf3(1, 0, 0))));

    std::size_t where = 0;
    REQUIRE(h.journal_barrier_after(0, &where));
    CHECK(where == 1);  // after the first command, before the second
    // Asking from beyond it reports none: a host that has already
    // re-snapshotted and moved its floor past the barrier is clear again.
    CHECK_FALSE(h.journal_barrier_after(where + 1, &at));
    // And the index is absolute, so it survives a trim the way every other
    // journal index does.
    h.trim_journal(1);
    std::size_t after_trim = 0;
    REQUIRE(h.journal_barrier_after(0, &after_trim));
    CHECK(after_trim == 1);
}

TEST_CASE("journal: an empty segment names no snapshot, so asking below the floor is not a refusal") {
    // A host that asks below the trimmed floor is told the events are gone by
    // getting none of them. Stamping that empty segment with the session's
    // snapshot would turn it into a wrong-snapshot refusal the first time it
    // were replayed onto anything else — the right answer to the wrong
    // question, and an empty journal cannot misapply anything anyway.
    World w;
    session::History h;
    h.set_enabled(true);
    h.note_snapshot(0x9999ull);
    REQUIRE(h.perform(w.doc, add_sphere(w.sdf, 0.5f, kernel::cf3(0, 0, 0))));
    std::size_t at = 0;
    (void)h.journal_since(0, &at);
    h.trim_journal(at);

    std::size_t ignored = 0;
    const std::vector<std::uint8_t> stale = h.journal_since(0, &ignored);
    World elsewhere;
    elsewhere.doc.snapshot_id = 0x7777ull;  // a different document entirely
    session::History replayer;
    replayer.set_enabled(true);
    session::History::ReplayResult res;
    REQUIRE(replayer.replay(stale.data(), stale.size(), elsewhere.doc, elsewhere.grid_for(),
                            elsewhere.mesh_for(), &res));
    CHECK(res.applied == 0);
    CHECK_FALSE(res.snapshot_mismatch);
}
