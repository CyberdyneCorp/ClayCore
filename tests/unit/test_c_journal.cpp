// CRASH RECOVERY ACROSS THE C ABI (c-abi spec, survive-a-crash).
//
// The cases in test_session_journal.cpp prove the LOG behaves; these prove the
// BOUNDARY does, and three of them cannot be reached from C++ at all:
//
//   * THE PAIRING, END TO END. The snapshot's identity is stamped by
//     clay_document_save_memory and clay_document_load_memory, so only a test
//     that goes through those two calls shows a host getting the check without
//     asking for it. A C++ case can set the field by hand and prove the
//     comparison; it cannot prove the stamp is there.
//   * THE TYPED REFUSAL. CLAY_ERROR_SNAPSHOT_MISMATCH and
//     CLAY_ERROR_INVALID_ARGUMENT mean opposite things to a host — one says
//     find the other snapshot, the other says discard the file — and the C++
//     API returns a bool for both.
//   * THE BARRIER, PROVOKED BY A HOST OPERATION. clay_voxel_drop_level is the
//     only entry point in the library that records one, and until this change
//     it recorded nothing: a journal replayed across a dropped resolution
//     level rebuilt a grid that still had the level, and said so nowhere.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay.h"

namespace {

clay_item_desc sphere_at(float r, float x) {
    clay_item_desc d{};
    d.struct_size = sizeof(d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = r;
    d.position[0] = x;
    d.rotation[3] = 1.0f;
    d.scale = 1.0f;
    d.op = CLAY_OP_ADD;
    d.blend = CLAY_BLEND_CUBIC;
    d.color[0] = 0.8f;
    d.color[1] = 0.7f;
    d.color[2] = 0.5f;
    return d;
}

// Six probes around the origin, which is where every fixture here builds.
const float kProbes[18] = {0.0f, 0.0f,  0.0f,  0.6f, 0.0f, 0.0f,  -0.6f, 0.0f, 0.0f,
                           0.0f, 0.45f, 0.0f,  1.1f, 0.0f, 0.0f,  0.0f,  0.0f, 0.9f};
constexpr std::size_t kProbeCount = 6;

std::vector<float> field_at_probes(const clay_document* doc) {
    std::vector<float> out(kProbeCount, 0.0f);
    REQUIRE(clay_eval_points(doc, nullptr, kProbes, kProbeCount, out.data(), nullptr) == CLAY_OK);
    return out;
}

std::vector<std::uint8_t> take(clay_blob* blob) {
    const std::uint8_t* p = clay_blob_data(blob);
    std::vector<std::uint8_t> out(p, p + clay_blob_size(blob));
    clay_blob_destroy(blob);
    return out;
}

// A document with an SDF layer and a voxel layer, a few edits made BEFORE the
// snapshot so the snapshot is not an empty document, and undo running.
struct Session {
    clay_document* doc = nullptr;
    clay_layer_id sdf = 0;
    clay_layer_id blocks = 0;
    clay_voxel_grid* grid = nullptr;

    Session() {
        doc = clay_document_create();
        REQUIRE(doc != nullptr);
        REQUIRE(clay_add_sdf_layer(doc, "blockout", &sdf) == CLAY_OK);
        REQUIRE(clay_document_add_voxel_layer(doc, "blocks", 0.1f, &blocks, &grid) == CLAY_OK);
        REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
    }
    ~Session() { clay_document_destroy(doc); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    clay_node_id add(float r, float x) {
        clay_node_id node = 0;
        const clay_item_desc d = sphere_at(r, x);
        REQUIRE(clay_add_item(doc, sdf, &d, &node) == CLAY_OK);
        return node;
    }
    void set_cell(int32_t x, int32_t y, int32_t z, int32_t index) {
        const int32_t cell[3] = {x, y, z};
        REQUIRE(clay_voxel_set(grid, cell, index) == CLAY_OK);
    }
};

std::size_t node_count(const clay_document* doc, clay_layer_id layer) {
    std::size_t n = 0;
    REQUIRE(clay_layer_node_count(doc, layer, &n) == CLAY_OK);
    return n;
}

std::size_t occupied(const clay_voxel_grid* grid) {
    std::size_t n = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &n) == CLAY_OK);
    return n;
}

}  // namespace

TEST_CASE("c abi: a session is reconstructed from a snapshot and a journal") {
    // The scenario the whole change exists for, taken through the two calls a
    // host actually has: a snapshot in memory, and the steps since it.
    Session s;
    s.add(0.5f, 0.0f);
    s.set_cell(0, 0, 0, 1);

    clay_blob* snapshot_blob = nullptr;
    REQUIRE(clay_document_save_memory(s.doc, &snapshot_blob) == CLAY_OK);
    const std::vector<std::uint8_t> snapshot = take(snapshot_blob);
    std::size_t at_snapshot = 0, journal_next = 0;
    REQUIRE(clay_document_journal_range(s.doc, nullptr, &at_snapshot) == CLAY_OK);

    // The edits a crash would otherwise cost, across both representations.
    s.add(0.3f, 0.6f);
    s.set_cell(1, 0, 0, 2);
    s.set_cell(0, 1, 0, 1);
    s.add(0.2f, -0.6f);

    clay_blob* journal_blob = nullptr;
    REQUIRE(clay_document_journal_since(s.doc, at_snapshot, &journal_blob, &journal_next) ==
            CLAY_OK);
    const std::vector<std::uint8_t> journal = take(journal_blob);
    CHECK(journal_next > at_snapshot);

    const std::vector<float> before = field_at_probes(s.doc);
    const std::size_t cells_before = occupied(s.grid);
    const std::size_t nodes_before = node_count(s.doc, s.sdf);

    // The recovery: the snapshot, reloaded, plus the steps since it.
    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(snapshot.data(), snapshot.size(), &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    std::size_t applied = 0;
    int32_t stopped = -1;
    REQUIRE(clay_document_replay_journal(recovered, journal.data(), journal.size(), &applied,
                                         &stopped) == CLAY_OK);
    CHECK(applied == journal_next - at_snapshot);
    CHECK(stopped == 0);

    // It evaluates identically at every probe point...
    const std::vector<float> after = field_at_probes(recovered);
    for (std::size_t i = 0; i < kProbeCount; ++i) CHECK(after[i] == doctest::Approx(before[i]));
    CHECK(node_count(recovered, s.sdf) == nodes_before);

    // ...and its voxel layer holds the same cells, with the same indices.
    clay_layer_id blocks = 0;
    clay_voxel_grid* rgrid = nullptr;
    REQUIRE(clay_document_voxel_layer(recovered, "blocks", &blocks, &rgrid) == CLAY_OK);
    CHECK(occupied(rgrid) == cells_before);
    const int32_t cells[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (const auto& cell : cells) {
        int32_t mine = -1, theirs = -2;
        REQUIRE(clay_voxel_get(s.grid, cell, &mine) == CLAY_OK);
        REQUIRE(clay_voxel_get(rgrid, cell, &theirs) == CLAY_OK);
        CHECK(mine == theirs);
    }

    clay_document_destroy(recovered);
}

TEST_CASE("c abi: the journal is incremental") {
    Session s;
    s.add(0.5f, 0.0f);
    clay_blob* snapshot_blob = nullptr;
    REQUIRE(clay_document_save_memory(s.doc, &snapshot_blob) == CLAY_OK);
    const std::vector<std::uint8_t> snapshot = take(snapshot_blob);
    std::size_t at = 0;
    REQUIRE(clay_document_journal_range(s.doc, nullptr, &at) == CLAY_OK);

    s.add(0.3f, 0.6f);
    clay_blob* first_blob = nullptr;
    std::size_t after_first = 0;
    REQUIRE(clay_document_journal_since(s.doc, at, &first_blob, &after_first) == CLAY_OK);
    const std::vector<std::uint8_t> first = take(first_blob);

    s.add(0.2f, -0.6f);
    s.set_cell(2, 0, 0, 1);
    clay_blob* second_blob = nullptr;
    std::size_t after_second = 0;
    REQUIRE(clay_document_journal_since(s.doc, after_first, &second_blob, &after_second) ==
            CLAY_OK);
    const std::vector<std::uint8_t> second = take(second_blob);
    CHECK(after_second > after_first);

    const std::vector<float> before = field_at_probes(s.doc);

    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(snapshot.data(), snapshot.size(), &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    std::size_t applied_first = 0, applied_second = 0;
    REQUIRE(clay_document_replay_journal(recovered, first.data(), first.size(), &applied_first,
                                         nullptr) == CLAY_OK);
    REQUIRE(clay_document_replay_journal(recovered, second.data(), second.size(), &applied_second,
                                         nullptr) == CLAY_OK);
    CHECK(applied_first == after_first - at);
    CHECK(applied_second == after_second - after_first);

    const std::vector<float> after = field_at_probes(recovered);
    for (std::size_t i = 0; i < kProbeCount; ++i) CHECK(after[i] == doctest::Approx(before[i]));
    CHECK(node_count(recovered, s.sdf) == node_count(s.doc, s.sdf));

    clay_document_destroy(recovered);
}

TEST_CASE("c abi: replay reports what it applied and stops at a step it cannot apply") {
    // A step that cannot be applied STOPS the replay. Skipping it would carry
    // on writing later events onto a document the earlier ones never reached,
    // which matches neither the snapshot nor the session.
    //
    // Neither document here was ever serialized, so neither journal names a
    // snapshot and the pairing check has nothing to say — this case is about
    // the other refusal.
    Session s;
    s.add(0.5f, 0.0f);       // event 0: a command
    s.set_cell(0, 0, 0, 1);  // event 1: a voxel run on the blocks layer
    s.add(0.3f, 0.6f);       // event 2: a command

    clay_blob* journal_blob = nullptr;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(s.doc, 0, &journal_blob, &now_at) == CLAY_OK);
    const std::vector<std::uint8_t> journal = take(journal_blob);

    // A document with the SDF layer and NO voxel layer, so the middle event
    // names a grid that is not there.
    clay_document* into = clay_document_create();
    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(into, "blockout", &sdf) == CLAY_OK);
    REQUIRE(sdf == s.sdf);  // same allocation order, so the events name it
    REQUIRE(clay_document_enable_undo(into) == CLAY_OK);

    std::size_t applied = 999;
    int32_t stopped = -1;
    CHECK(clay_document_replay_journal(into, journal.data(), journal.size(), &applied, &stopped) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(applied == 1);   // the command before it, and nothing after
    CHECK(stopped == 0);   // not a barrier: this one is a refusal
    CHECK(node_count(into, sdf) == 1);

    clay_document_destroy(into);
}

TEST_CASE("c abi: an unreadable journal is refused and the document is left as it was") {
    Session s;
    s.add(0.5f, 0.0f);
    clay_blob* snapshot_blob = nullptr;
    REQUIRE(clay_document_save_memory(s.doc, &snapshot_blob) == CLAY_OK);
    const std::vector<std::uint8_t> snapshot = take(snapshot_blob);
    std::size_t at = 0;
    REQUIRE(clay_document_journal_range(s.doc, nullptr, &at) == CLAY_OK);
    s.add(0.3f, 0.6f);
    clay_blob* journal_blob = nullptr;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(s.doc, at, &journal_blob, &now_at) == CLAY_OK);
    const std::vector<std::uint8_t> journal = take(journal_blob);
    REQUIRE(journal.size() > 24);

    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(snapshot.data(), snapshot.size(), &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    const std::vector<float> before = field_at_probes(recovered);
    const std::size_t nodes_before = node_count(recovered, s.sdf);

    SUBCASE("a journal from a newer build") {
        std::vector<std::uint8_t> newer = journal;
        newer[4] = 99;  // the version field
        std::size_t applied = 999;
        CHECK(clay_document_replay_journal(recovered, newer.data(), newer.size(), &applied,
                                           nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(applied == 0);
    }
    SUBCASE("a truncated one") {
        std::size_t applied = 999;
        CHECK(clay_document_replay_journal(recovered, journal.data(), journal.size() - 6, &applied,
                                           nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("and one that is not a journal at all") {
        const std::uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::size_t applied = 999;
        CHECK(clay_document_replay_journal(recovered, junk, sizeof(junk), &applied, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(applied == 0);
    }

    const std::vector<float> after = field_at_probes(recovered);
    for (std::size_t i = 0; i < kProbeCount; ++i) CHECK(after[i] == doctest::Approx(before[i]));
    CHECK(node_count(recovered, s.sdf) == nodes_before);

    clay_document_destroy(recovered);
}

TEST_CASE("c abi: a journal paired with the wrong snapshot is refused, not replayed") {
    // 2.1, end to end. Nothing here asks for the identity: the snapshot side
    // stamps it in clay_document_save_memory and the recovery side in
    // clay_document_load_memory, so a host gets the refusal by writing the
    // ordinary recovery path.
    Session mine;
    mine.add(0.5f, 0.0f);
    clay_blob* mine_blob = nullptr;
    REQUIRE(clay_document_save_memory(mine.doc, &mine_blob) == CLAY_OK);
    const std::vector<std::uint8_t> my_snapshot = take(mine_blob);
    std::size_t at = 0;
    REQUIRE(clay_document_journal_range(mine.doc, nullptr, &at) == CLAY_OK);
    mine.add(0.3f, 0.6f);
    mine.set_cell(1, 1, 1, 1);
    clay_blob* journal_blob = nullptr;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(mine.doc, at, &journal_blob, &now_at) == CLAY_OK);
    const std::vector<std::uint8_t> journal = take(journal_blob);

    // A DIFFERENT session, snapshotted separately. Its layer ids line up with
    // the journal's, which is exactly why replaying onto it would otherwise
    // succeed and hand back a document matching neither.
    Session other;
    other.add(0.8f, 0.2f);
    clay_blob* other_blob = nullptr;
    REQUIRE(clay_document_save_memory(other.doc, &other_blob) == CLAY_OK);
    const std::vector<std::uint8_t> other_snapshot = take(other_blob);

    clay_document* wrong = nullptr;
    REQUIRE(clay_document_load_memory(other_snapshot.data(), other_snapshot.size(), &wrong) ==
            CLAY_OK);
    REQUIRE(clay_document_enable_undo(wrong) == CLAY_OK);
    const std::vector<float> before = field_at_probes(wrong);
    std::size_t applied = 999;
    int32_t stopped = -1;
    CHECK(clay_document_replay_journal(wrong, journal.data(), journal.size(), &applied, &stopped) ==
          CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(applied == 0);
    CHECK(node_count(wrong, mine.sdf) == 1);  // nothing applied
    const std::vector<float> after = field_at_probes(wrong);
    for (std::size_t i = 0; i < kProbeCount; ++i) CHECK(after[i] == doctest::Approx(before[i]));
    clay_document_destroy(wrong);

    // A document that was never serialized cannot be the snapshot either.
    clay_document* scratch = clay_document_create();
    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(scratch, "blockout", &sdf) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(scratch) == CLAY_OK);
    CHECK(clay_document_replay_journal(scratch, journal.data(), journal.size(), &applied,
                                       nullptr) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(node_count(scratch, sdf) == 0);
    clay_document_destroy(scratch);

    // And onto the snapshot it WAS taken against, the same bytes replay.
    clay_document* right = nullptr;
    REQUIRE(clay_document_load_memory(my_snapshot.data(), my_snapshot.size(), &right) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(right) == CLAY_OK);
    REQUIRE(clay_document_replay_journal(right, journal.data(), journal.size(), &applied,
                                         nullptr) == CLAY_OK);
    CHECK(applied == now_at - at);
    CHECK(node_count(right, mine.sdf) == node_count(mine.doc, mine.sdf));
    clay_document_destroy(right);
}

TEST_CASE("c abi: a barrier tells the host to re-snapshot, and stops a replay") {
    // Both halves of "a journal says when it stops being enough", provoked by
    // the one host operation that records a barrier. Rule 1 is the half that
    // had no mechanism: replay reports a barrier, but replay happens during
    // the recovery, which is the one moment when being told to take a fresher
    // snapshot is useless.
    Session s;
    s.add(0.5f, 0.0f);
    clay_blob* snapshot_blob = nullptr;
    REQUIRE(clay_document_save_memory(s.doc, &snapshot_blob) == CLAY_OK);
    const std::vector<std::uint8_t> snapshot = take(snapshot_blob);
    std::size_t at = 0;
    REQUIRE(clay_document_journal_range(s.doc, nullptr, &at) == CLAY_OK);

    s.add(0.3f, 0.6f);
    int32_t has_barrier = -1;
    std::size_t where = 999;
    REQUIRE(clay_document_journal_barrier(s.doc, at, &has_barrier, &where) == CLAY_OK);
    CHECK(has_barrier == 0);
    CHECK(where == 999);  // untouched when there is none

    // Dropping a resolution level destroys detail nothing can reproduce.
    std::size_t level = 0;
    REQUIRE(clay_voxel_add_level(s.grid, &level) == CLAY_OK);
    REQUIRE(clay_voxel_drop_level(s.grid) == CLAY_OK);
    s.add(0.2f, -0.6f);

    REQUIRE(clay_document_journal_barrier(s.doc, at, &has_barrier, &where) == CLAY_OK);
    CHECK(has_barrier == 1);
    CHECK(where > at);
    // The host learns it here, while it can still take a snapshot.
    int32_t past = -1;
    REQUIRE(clay_document_journal_barrier(s.doc, where + 1, &past, nullptr) == CLAY_OK);
    CHECK(past == 0);

    clay_blob* journal_blob = nullptr;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(s.doc, at, &journal_blob, &now_at) == CLAY_OK);
    const std::vector<std::uint8_t> journal = take(journal_blob);

    clay_document* recovered = nullptr;
    REQUIRE(clay_document_load_memory(snapshot.data(), snapshot.size(), &recovered) == CLAY_OK);
    REQUIRE(clay_document_enable_undo(recovered) == CLAY_OK);
    std::size_t applied = 0;
    int32_t stopped = -1;
    REQUIRE(clay_document_replay_journal(recovered, journal.data(), journal.size(), &applied,
                                         &stopped) == CLAY_OK);
    CHECK(stopped == 1);
    CHECK(applied == where - at);  // everything up to the barrier, and no more
    // The edit AFTER the barrier is not in the recovered document: replay
    // stopped rather than skipping, so what is missing is visible instead of
    // silent.
    CHECK(node_count(recovered, s.sdf) < node_count(s.doc, s.sdf));

    clay_document_destroy(recovered);
}
