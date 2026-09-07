#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

// A mesh layer's geometry revision, across every path that replaces its
// triangles wholesale (issue #472).
//
// The counter existed before this file and moved for exactly two callers —
// the document-side replace and the layer weld. History was not one of them:
// `session::History` restores a mesh through a `mesh::Mesh*` resolver, so undo,
// redo and journal replay swapped every vertex and every index and left the
// revision where it was. A host that rebuilt, undid, and kept sculpting got a
// refused stroke on the next dab with nothing naming the cause.
//
// What is asserted here is the pair, at every transition: the revision STRICTLY
// ADVANCES and the triangles are the ones expected. Either alone would pass for
// the wrong reason — a counter that moves over unchanged geometry is noise, and
// geometry that is right under a frozen counter is exactly the bug.

namespace {

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

struct DocHandle {
    clay_document* doc = nullptr;
    ~DocHandle() { clay_document_destroy(doc); }
    DocHandle() = default;
    DocHandle(const DocHandle&) = delete;
    DocHandle& operator=(const DocHandle&) = delete;
};

struct BlobHandle {
    clay_blob* b = nullptr;
    ~BlobHandle() { clay_blob_destroy(b); }
    BlobHandle() = default;
    BlobHandle(const BlobHandle&) = delete;
    BlobHandle& operator=(const BlobHandle&) = delete;
};

// A UV sphere, built here rather than shared with the C++ fixtures: this file
// speaks only the C header, which is the point of it. Two different sizes are
// what the expectations below are derived from — the counts come from the
// vectors this fills, never from a call into the library.
struct TriMesh {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::size_t triangles() const { return indices.size() / 3; }
};

TriMesh sphere(float radius, int rings, int segments) {
    TriMesh out;
    const float pi = 3.14159265358979323846f;
    auto push = [&](float x, float y, float z) {
        out.positions.push_back(x);
        out.positions.push_back(y);
        out.positions.push_back(z);
    };
    push(0.0f, radius, 0.0f);
    for (int r = 1; r < rings; ++r) {
        const float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        const float y = std::cos(phi) * radius, rr = std::sin(phi) * radius;
        for (int s = 0; s < segments; ++s) {
            const float th = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            push(rr * std::cos(th), y, rr * std::sin(th));
        }
    }
    push(0.0f, -radius, 0.0f);
    const std::uint32_t top = 0;
    const std::uint32_t bottom = static_cast<std::uint32_t>(out.positions.size() / 3 - 1);
    auto at = [&](int r, int s) {
        return static_cast<std::uint32_t>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        out.indices.push_back(top);
        out.indices.push_back(at(1, s + 1));
        out.indices.push_back(at(1, s));
    }
    for (int r = 1; r < rings - 1; ++r)
        for (int s = 0; s < segments; ++s) {
            out.indices.push_back(at(r, s));
            out.indices.push_back(at(r, s + 1));
            out.indices.push_back(at(r + 1, s));
            out.indices.push_back(at(r, s + 1));
            out.indices.push_back(at(r + 1, s + 1));
            out.indices.push_back(at(r + 1, s));
        }
    for (int s = 0; s < segments; ++s) {
        out.indices.push_back(bottom);
        out.indices.push_back(at(rings - 1, s));
        out.indices.push_back(at(rings - 1, s + 1));
    }
    return out;
}

clay_result build(MeshHandle* out, const TriMesh& m) {
    return clay_mesh_from_triangles(m.positions.data(), m.positions.size() / 3,
                                    m.indices.data(), m.indices.size(), &out->m);
}

clay_layer_id attach_layer(DocHandle* doc, const TriMesh& m, const char* name = "shell") {
    MeshHandle src;
    REQUIRE(build(&src, m) == CLAY_OK);
    clay_mesh_layer_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = name;
    clay_layer_id id = 0;
    REQUIRE(clay_document_add_mesh_layer(doc->doc, src.m, &desc, &id, nullptr) == CLAY_OK);
    return id;
}

std::uint64_t revision(clay_document* doc, clay_layer_id layer) {
    std::uint64_t r = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc, layer, &r) == CLAY_OK);
    return r;
}

// The layer's triangles, read back through the ABI and compared against the
// arrays the test itself built. Positions as well as counts: a rebuild that
// happened to land on the same counts is exactly the case the revision exists
// for, so a count-only comparison would be blind to it here too.
bool layer_holds(clay_document* doc, clay_layer_id layer, const TriMesh& expected) {
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc, layer, &borrowed) == CLAY_OK);
    if (clay_mesh_index_count(borrowed) != expected.indices.size()) return false;
    if (clay_mesh_vertex_count(borrowed) != expected.positions.size() / 3) return false;
    const float* p = clay_mesh_positions(borrowed);
    if (!p) return false;
    return std::memcmp(p, expected.positions.data(),
                       expected.positions.size() * sizeof(float)) == 0;
}

// Replace a layer's triangles with `m`, unconditionally. Its own helper so the
// tests below read as the sequence they are testing.
clay_result replace_with(clay_document* doc, clay_layer_id layer, const TriMesh& m,
                         std::uint64_t expected_revision = 0) {
    MeshHandle src;
    REQUIRE(build(&src, m) == CLAY_OK);
    return clay_document_replace_mesh_layer(doc, layer, src.m, expected_revision);
}

}  // namespace

TEST_CASE("c abi: the mesh geometry revision advances through attach, rebuild, undo and redo") {
    const TriMesh fine = sphere(1.0f, 8, 16);
    const TriMesh coarse = sphere(0.5f, 4, 8);
    // The fixture is only worth anything if the two meshes differ, and differ
    // in a way a count comparison can see as well as a byte comparison.
    REQUIRE(fine.triangles() == 224u);
    REQUIRE(coarse.triangles() == 48u);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    REQUIRE(clay_document_enable_undo(doc.doc) == CLAY_OK);
    const clay_layer_id layer = attach_layer(&doc, fine);

    // Attaching triangles is the first wholesale install, and the generation
    // starts there.
    const std::uint64_t attached = revision(doc.doc, layer);
    CHECK(attached == 1u);
    CHECK(layer_holds(doc.doc, layer, fine));

    REQUIRE(replace_with(doc.doc, layer, coarse) == CLAY_OK);
    const std::uint64_t rebuilt = revision(doc.doc, layer);
    CHECK(rebuilt == 2u);
    CHECK(layer_holds(doc.doc, layer, coarse));

    // THE BUG. Undo puts every one of the old vertices and indices back, which
    // is precisely the change a cached adjacency, a BVH or a live sculptor does
    // not survive — so the token a host holds against those caches has to move.
    // It does NOT go back to 1: the number is an invalidation token for a live
    // cache, not the age of the restored mesh.
    std::int32_t moved = 0;
    REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
    REQUIRE(moved == 1);
    CHECK(layer_holds(doc.doc, layer, fine));
    const std::uint64_t undone = revision(doc.doc, layer);
    CHECK(undone == 3u);

    REQUIRE(clay_document_redo(doc.doc, &moved) == CLAY_OK);
    REQUIRE(moved == 1);
    CHECK(layer_holds(doc.doc, layer, coarse));
    const std::uint64_t redone = revision(doc.doc, layer);
    CHECK(redone == 4u);

    // Repeated cycles: every transition is a new generation, and none of them
    // repeats a number a host may still be holding.
    std::uint64_t last = redone;
    for (int cycle = 0; cycle < 3; ++cycle) {
        REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
        REQUIRE(moved == 1);
        CHECK(layer_holds(doc.doc, layer, fine));
        const std::uint64_t back = revision(doc.doc, layer);
        CHECK(back > last);
        last = back;

        REQUIRE(clay_document_redo(doc.doc, &moved) == CLAY_OK);
        REQUIRE(moved == 1);
        CHECK(layer_holds(doc.doc, layer, coarse));
        const std::uint64_t forward = revision(doc.doc, layer);
        CHECK(forward > last);
        last = forward;
    }
    CHECK(last == 10u);
}

TEST_CASE("c abi: a replayed journal advances the mesh geometry revision too") {
    // The SECOND path into the replacement, and the one that says where the
    // bump belongs. A bump written into clay_document_undo would pass the case
    // above and leave this one exactly as broken as it was: replay reaches the
    // same restore through History::replay, not through undo.
    const TriMesh fine = sphere(1.0f, 8, 16);
    const TriMesh coarse = sphere(0.5f, 4, 8);

    DocHandle source;
    source.doc = clay_document_create();
    REQUIRE(source.doc != nullptr);
    const clay_layer_id layer = attach_layer(&source, fine);
    // Undo is enabled AFTER the attach, so the journal starts empty and holds
    // the replacement alone — which is what lets a second document that already
    // holds the same layer replay it.
    REQUIRE(clay_document_enable_undo(source.doc) == CLAY_OK);
    REQUIRE(replace_with(source.doc, layer, coarse) == CLAY_OK);

    BlobHandle journal;
    std::size_t now_at = 0;
    REQUIRE(clay_document_journal_since(source.doc, 0, &journal.b, &now_at) == CLAY_OK);
    REQUIRE(clay_blob_size(journal.b) > 0);

    DocHandle replayed;
    replayed.doc = clay_document_create();
    REQUIRE(replayed.doc != nullptr);
    const clay_layer_id same = attach_layer(&replayed, fine);
    REQUIRE(same == layer);  // ids are monotonic, so the two documents agree
    REQUIRE(clay_document_enable_undo(replayed.doc) == CLAY_OK);
    REQUIRE(revision(replayed.doc, same) == 1u);

    std::size_t applied = 0;
    std::int32_t stopped = 0;
    REQUIRE(clay_document_replay_journal(replayed.doc, clay_blob_data(journal.b),
                                         clay_blob_size(journal.b), &applied,
                                         &stopped) == CLAY_OK);
    CHECK(applied == 1u);
    CHECK(stopped == 0);
    CHECK(layer_holds(replayed.doc, same, coarse));
    CHECK(revision(replayed.doc, same) == 2u);

    // ...and the replayed step is a real step: undoing it restores the original
    // triangles and advances the token again.
    std::int32_t moved = 0;
    REQUIRE(clay_document_undo(replayed.doc, &moved) == CLAY_OK);
    REQUIRE(moved == 1);
    CHECK(layer_holds(replayed.doc, same, fine));
    CHECK(revision(replayed.doc, same) == 3u);
}

TEST_CASE("c abi: the mesh geometry revision stands still for everything that is not a replacement") {
    // A fix that bumps on everything is not a fix: the whole value of the token
    // is that a host can keep an adjacency, a BVH or a live sculptor across the
    // changes that do not invalidate them.
    const TriMesh fine = sphere(1.0f, 8, 16);
    const TriMesh coarse = sphere(0.5f, 4, 8);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    REQUIRE(clay_document_enable_undo(doc.doc) == CLAY_OK);
    const clay_layer_id layer = attach_layer(&doc, fine, "kept");
    const clay_layer_id other = attach_layer(&doc, fine, "other");
    const std::uint64_t start = revision(doc.doc, layer);
    REQUIRE(start == 1u);

    SUBCASE("a vertex-position sculpt does not move it") {
        clay_mesh* borrowed = nullptr;
        REQUIRE(clay_document_mesh_layer_by_id(doc.doc, layer, &borrowed) == CLAY_OK);
        clay_mesh_sculptor* sculptor = nullptr;
        REQUIRE(clay_mesh_sculptor_create(borrowed, -1.0f, &sculptor) == CLAY_OK);
        clay_mesh_brush_desc brush{};
        brush.struct_size = sizeof(brush);
        REQUIRE(clay_mesh_brush_defaults(&brush) == CLAY_OK);
        brush.verb = CLAY_MESH_BRUSH_DRAW;
        brush.center[0] = 0.0f;
        brush.center[1] = 1.0f;
        brush.center[2] = 0.0f;
        brush.radius = 0.4f;
        brush.strength = 0.05f;
        std::size_t moved = 0;
        REQUIRE(clay_mesh_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &moved) == CLAY_OK);
        CHECK(moved > 0);  // the fixture actually moved vertices
        CHECK(revision(doc.doc, layer) == start);
        clay_mesh_sculptor_destroy(sculptor);
    }

    SUBCASE("a rename does not move it") {
        REQUIRE(clay_document_set_layer_name(doc.doc, layer, "renamed") == CLAY_OK);
        CHECK(revision(doc.doc, layer) == start);
        // ...nor does undoing the rename, which is history touching the layer
        // without touching a triangle.
        std::int32_t moved = 0;
        REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
        REQUIRE(moved == 1);
        CHECK(revision(doc.doc, layer) == start);
    }

    SUBCASE("visibility does not move it") {
        REQUIRE(clay_document_set_layer_visible(doc.doc, layer, 0) == CLAY_OK);
        CHECK(revision(doc.doc, layer) == start);
        REQUIRE(clay_document_set_layer_visible(doc.doc, layer, 1) == CLAY_OK);
        CHECK(revision(doc.doc, layer) == start);
    }

    SUBCASE("a transform-only change does not move it") {
        const float position[3] = {0.5f, 0.25f, -0.75f};
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        REQUIRE(clay_document_set_layer_transform(doc.doc, layer, position, axis, 0.4f, 2.0f) ==
                CLAY_OK);
        CHECK(revision(doc.doc, layer) == start);
        std::int32_t moved = 0;
        REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
        REQUIRE(moved == 1);
        CHECK(revision(doc.doc, layer) == start);
    }

    SUBCASE("history affecting another layer does not move it") {
        REQUIRE(replace_with(doc.doc, other, coarse) == CLAY_OK);
        CHECK(revision(doc.doc, other) == 2u);
        CHECK(revision(doc.doc, layer) == start);
        std::int32_t moved = 0;
        REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
        REQUIRE(moved == 1);
        CHECK(layer_holds(doc.doc, other, fine));
        CHECK(revision(doc.doc, other) == 3u);
        CHECK(revision(doc.doc, layer) == start);  // the neighbour is untouched
        REQUIRE(clay_document_redo(doc.doc, &moved) == CLAY_OK);
        REQUIRE(moved == 1);
        CHECK(revision(doc.doc, other) == 4u);
        CHECK(revision(doc.doc, layer) == start);
    }

    SUBCASE("a refused replacement does not move it") {
        // Stale: the caller's token is older than the layer.
        REQUIRE(replace_with(doc.doc, layer, coarse, start) == CLAY_OK);
        const std::uint64_t now = revision(doc.doc, layer);
        REQUIRE(now == 2u);
        CHECK(replace_with(doc.doc, layer, fine, start) == CLAY_ERROR_FORWARD_VERSION);
        CHECK(revision(doc.doc, layer) == now);
        CHECK(layer_holds(doc.doc, layer, coarse));

        // Locked: the layer takes no edits at all.
        REQUIRE(clay_document_set_layer_protection(doc.doc, layer, 0, 1) == CLAY_OK);
        CHECK(replace_with(doc.doc, layer, fine, 0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(revision(doc.doc, layer) == now);
        CHECK(layer_holds(doc.doc, layer, coarse));
        REQUIRE(clay_document_set_layer_protection(doc.doc, layer, 0, 0) == CLAY_OK);
    }
}

TEST_CASE("c abi: an undo with nothing to undo spends no mesh generation") {
    // The empty stack is reported rather than failed, and a report is not a
    // replacement — a UI that drives the button without tracking state must not
    // invalidate a host's caches once per click.
    const TriMesh fine = sphere(1.0f, 8, 16);
    const TriMesh coarse = sphere(0.5f, 4, 8);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    const clay_layer_id layer = attach_layer(&doc, fine);
    // Enabled AFTER the attach, so the replacement below is the only step there
    // is and the stack empties without the layer going with it.
    REQUIRE(clay_document_enable_undo(doc.doc) == CLAY_OK);
    REQUIRE(replace_with(doc.doc, layer, coarse) == CLAY_OK);

    std::int32_t moved = 0;
    REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
    REQUIRE(moved == 1);
    CHECK(layer_holds(doc.doc, layer, fine));
    const std::uint64_t drained = revision(doc.doc, layer);
    CHECK(drained == 3u);

    REQUIRE(clay_document_undo(doc.doc, &moved) == CLAY_OK);
    CHECK(moved == 0);
    CHECK(revision(doc.doc, layer) == drained);
    CHECK(layer_holds(doc.doc, layer, fine));
}
