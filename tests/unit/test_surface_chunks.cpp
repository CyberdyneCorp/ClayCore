// ONE CHUNK UNIT, AND WHAT IT PROMISES (sculpt-runtime spec,
// add-extreme-poly-runtime).
//
// The requirement this file defends is that a change tracked at one granularity
// never has to be translated into another before anything can act on it. That
// is not observable from a picture of the surface, so it is asserted directly:
//
//   1. EVERY FACE IS IN EXACTLY ONE CHUNK, on both partitioners. A face in none
//      is invisible to every query; a face in two is counted twice, and the
//      second sighting is what over-reports a dirty set.
//   2. THE REVISIONS SEPARATE. Geometry moving must not advance the topology
//      revision, because a host that cannot tell those apart re-uploads an
//      index buffer on every dab.
//   3. THE ACKNOWLEDGEMENT IS CONDITIONAL. A chunk that changed again between
//      the copy and the acknowledgement stays dirty — the one thing an
//      all-or-nothing `clear_dirty` structurally cannot express.
//   4. THE ARENA IS ONE ALLOCATION. A chunk that splits does not become a heap
//      object, which is what makes the allocation gate reachable at all.
//
// The scaling gates themselves (7.3, 7.4, 7.5) are a separate file at sizes CI
// can afford; this one is about the unit being one unit.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/memory/capacity.h"
#include "clay/memory/scratch.h"
#include "clay/mesh/maintenance.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/preflight.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using mesh::ChunkDirty;
using mesh::ChunkOptions;
using mesh::ChunkTable;
using mesh::Mesh;
using mesh::MultiresSurface;
using mesh::SurfaceChunk;

namespace {

Mesh grid_triangles(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const auto at = [&](int x, int z) {
        return static_cast<std::uint32_t>(z * (n + 1) + x);
    };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.indices.push_back(at(x, z));
            m.indices.push_back(at(x + 1, z));
            m.indices.push_back(at(x + 1, z + 1));
            m.indices.push_back(at(x, z));
            m.indices.push_back(at(x + 1, z + 1));
            m.indices.push_back(at(x, z + 1));
        }
    return m;
}

Mesh grid_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const auto at = [&](int x, int z) {
        return static_cast<std::uint32_t>(z * (n + 1) + x);
    };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.quads.push_back(at(x, z));
            m.quads.push_back(at(x + 1, z));
            m.quads.push_back(at(x + 1, z + 1));
            m.quads.push_back(at(x, z + 1));
            m.indices.push_back(at(x, z));
            m.indices.push_back(at(x + 1, z));
            m.indices.push_back(at(x + 1, z + 1));
            m.indices.push_back(at(x, z));
            m.indices.push_back(at(x + 1, z + 1));
            m.indices.push_back(at(x, z + 1));
        }
    return m;
}

}  // namespace

TEST_CASE("surface chunks: a mesh partition covers every triangle exactly once") {
    const Mesh mesh = grid_triangles(32, 1.0f);
    ChunkOptions options;
    options.target_faces = 64;
    ChunkTable table;
    mesh::partition_mesh_chunks(mesh, options, &table);

    std::vector<int> seen(mesh.triangle_count(), 0);
    std::size_t chunks = 0;
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        const SurfaceChunk* c = table.chunk(i);
        if (c == nullptr) continue;
        ++chunks;
        CHECK(c->faces.size() <= options.target_faces);
        CHECK_FALSE(c->faces.empty());
        for (mesh::FaceId f : c->faces) {
            REQUIRE(f.slot < seen.size());
            ++seen[f.slot];
        }
        // The chunk-local vertex map is ascending and unique, which is what
        // makes `local_of` a binary search rather than a per-chunk hash map.
        const mesh::ChunkVertexSpan vertices = table.vertices(i);
        REQUIRE_FALSE(vertices.empty());
        for (std::size_t k = 1; k < vertices.size(); ++k) CHECK(vertices[k - 1] < vertices[k]);
        CHECK(vertices.local_of(vertices[0]) == 0u);
        CHECK(vertices.local_of(0xfffffffeu) == mesh::ChunkVertexSpan::kNoLocal);
    }
    CHECK(chunks > 1);
    for (int n : seen) CHECK(n == 1);

    // A FRESH PARTITION IS NOT A CHANGE a host has to redraw: it is the state
    // it is about to read for the first time.
    CHECK(table.dirty().empty());
}

TEST_CASE("surface chunks: the four revisions separate") {
    const Mesh mesh = grid_triangles(8, 1.0f);
    ChunkTable table;
    mesh::partition_mesh_chunks(mesh, ChunkOptions{}, &table);
    REQUIRE(table.slot_count() >= 1);

    const mesh::ChunkRevisions before = table.chunk(0)->revisions;
    table.mark(0, ChunkDirty::Geometry);
    const mesh::ChunkRevisions after = table.chunk(0)->revisions;

    // The one distinction a host re-uploads an index buffer on.
    CHECK(after.geometry > before.geometry);
    CHECK(after.topology == before.topology);
    CHECK(after.normals == before.normals);
    CHECK(after.attributes == before.attributes);
    CHECK(table.chunk(0)->geometry_dirty);
    CHECK_FALSE(table.chunk(0)->topology_dirty);
    CHECK(table.dirty().size() == 1);

    // The epoch mark: touching the same chunk again does not grow the list.
    table.mark(0, ChunkDirty::Normals);
    CHECK(table.dirty().size() == 1);
    CHECK(table.chunk(0)->revisions.normals > after.normals);
}

TEST_CASE("surface chunks: an acknowledgement retires only what has not moved on") {
    const Mesh mesh = grid_triangles(8, 1.0f);
    ChunkOptions options;
    options.target_faces = 32;
    ChunkTable table;
    mesh::partition_mesh_chunks(mesh, options, &table);
    REQUIRE(table.slot_count() >= 2);

    table.mark(0, ChunkDirty::Geometry);
    table.mark(1, ChunkDirty::Geometry);
    REQUIRE(table.dirty().size() == 2);

    // What a host copied, for chunk 0.
    const mesh::ChunkRevisions copied = table.chunk(0)->revisions;
    // ...and then the engine moves on, for chunk 1 only.
    const mesh::ChunkRevisions stale = table.chunk(1)->revisions;
    table.mark(1, ChunkDirty::Geometry);

    CHECK(table.acknowledge(0, copied));
    // Chunk 1 changed between the copy and the acknowledgement, so it STAYS
    // dirty. This is the case `clear_dirty` cannot express: a host that drained
    // half a set and dropped a frame either re-uploads everything or loses a
    // change.
    CHECK_FALSE(table.acknowledge(1, stale));
    REQUIRE(table.dirty().size() == 1);
    CHECK(table.dirty()[0] == 1u);
    CHECK(table.acknowledge(1, table.chunk(1)->revisions));
    CHECK(table.dirty().empty());
}

TEST_CASE("surface chunks: a multires level partitions into stable, bounded chunks") {
    const Mesh cage = grid_quads(4, 1.0f);
    auto surface = MultiresSurface::from_mesh(cage);
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());

    const std::uint32_t level = surface->max_level();
    const ChunkTable& table = surface->chunks_at(level);
    const mesh::LevelTopology& topology = surface->topology_at(level);

    std::vector<int> seen(topology.face_count, 0);
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        const SurfaceChunk* c = table.chunk(i);
        if (c == nullptr) continue;
        // Bounded ABOVE, which is the property a base patch does not have:
        // subdivision quadruples a patch's faces per level.
        CHECK(c->faces.size() <= table.options().target_faces);
        for (mesh::FaceId f : c->faces) {
            REQUIRE(f.slot < seen.size());
            ++seen[f.slot];
        }
    }
    for (int n : seen) CHECK(n == 1);

    // The SAME partition after a second call: a chunk id a host uploaded by
    // must not mean a different set of faces the next time it asks.
    const std::size_t chunks = table.slot_count();
    const ChunkTable& again = surface->chunks_at(level);
    CHECK(again.slot_count() == chunks);
}

TEST_CASE("surface chunks: a multires stamp dirties the chunks it touched and no others") {
    const Mesh cage = grid_quads(8, 1.0f);
    auto surface = MultiresSurface::from_mesh(cage);
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());

    const std::uint32_t level = surface->max_level();
    const std::size_t chunks = surface->chunks_at(level).slot_count();
    REQUIRE(chunks > 4);
    surface->clear_dirty_chunks(level);
    REQUIRE(surface->chunks_at(level).dirty().empty());

    // One vertex, written the way the sculptor writes: through the level mesh
    // and then absorbed.
    Mesh& level_mesh = surface->level_mesh(level);
    REQUIRE(level_mesh.positions.size() > 8);
    level_mesh.positions[7].y += 0.25f;
    surface->absorb_level_edit(level, {7u});
    surface->positions_at(level);

    const std::size_t dirty = surface->chunks_at(level).dirty().size();
    CAPTURE(dirty);
    CAPTURE(chunks);
    CHECK(dirty > 0);
    // A LOCAL edit dirties a small fraction of the level. The exact number
    // depends on the partition; what must never happen is the whole level.
    CHECK(dirty * 4 < chunks);
}

TEST_CASE("surface chunks: the transport copies a chunk and reports staleness") {
    const Mesh cage = grid_quads(4, 1.0f);
    auto surface = MultiresSurface::from_mesh(cage);
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    const std::uint32_t level = surface->max_level();

    mesh::SurfaceView view = mesh::SurfaceView::over_level(*surface, level);
    REQUIRE(view.valid());
    REQUIRE(view.chunk_count() >= 1);

    // THE CAPACITY QUERY writes nothing and says what the chunk needs.
    const mesh::ChunkReadback sized =
        view.copy_chunk(0, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
    REQUIRE(sized.ok);
    CHECK(sized.vertex_count > 0);
    CHECK(sized.index_count % 3 == 0);

    std::vector<float> positions(static_cast<std::size_t>(sized.vertex_count) * 3, 0.0f);
    std::vector<std::uint32_t> indices(sized.index_count, 0u);
    const mesh::ChunkRevisions asked = view.chunks().chunk(0)->revisions;
    const mesh::ChunkReadback copied =
        view.copy_chunk(0, &asked, positions.data(), positions.size(), nullptr, 0, indices.data(),
                        indices.size());
    REQUIRE(copied.ok);
    CHECK_FALSE(copied.truncated);
    CHECK_FALSE(copied.stale);
    for (std::uint32_t i : indices) CHECK(i < sized.vertex_count);

    // A BUFFER TOO SMALL WRITES NOTHING — not a partial fill a caller might
    // draw — and says what it needed.
    const mesh::ChunkReadback short_buffer =
        view.copy_chunk(0, &asked, positions.data(), positions.size() - 1, nullptr, 0, nullptr, 0);
    CHECK(short_buffer.truncated);
    CHECK(short_buffer.vertex_count == sized.vertex_count);

    // And a readback taken against an older revision says so, rather than
    // being merely wrong.
    mesh::ChunkRevisions old = asked;
    old.topology = 0;
    const mesh::ChunkReadback stale =
        view.copy_chunk(0, &old, positions.data(), positions.size(), nullptr, 0, nullptr, 0);
    CHECK(stale.stale);
    CHECK(stale.current == asked);
}

TEST_CASE("memory: the capacity estimator refuses an overflow rather than sizing it") {
    memory::CapacityBuilder b;
    // Twenty million vertices is a real number for this change; the stride is
    // not. The point is the SHAPE of the bug: a wrapped estimate is a small
    // one, and a small one is allowed.
    b.authoritative(0xffffffffffffffffull, 2);
    const memory::CapacityEstimate estimate = b.finish();
    CHECK_FALSE(estimate.allowed);
    CHECK(estimate.error == memory::BudgetError::Overflow);
    CHECK(estimate.peak_bytes == 0);

    memory::CapacityBuilder ok;
    ok.authoritative(1000, 64);
    ok.transient(1000, 16);
    const memory::CapacityEstimate fits = ok.finish(0);
    CHECK(fits.allowed);
    CHECK(fits.persistent_bytes == 64000u);
    CHECK(fits.peak_bytes == 80000u);
    // THE PEAK IS WHAT IS COMPARED, not the result: an operation whose result
    // fits and whose high-water mark does not is the failure the refusal exists
    // for.
    CHECK_FALSE(ok.finish(70000).allowed);
    CHECK(ok.finish(70000).error == memory::BudgetError::OverBudget);
}

TEST_CASE("memory: the scratch arena bounds a stamp and reports its high-water mark") {
    memory::SculptMemoryProfile profile;
    profile.memory_class = memory::MemoryClass::Constrained;
    profile.scratch_budget = 4096;
    memory::ScratchArena arena;
    arena.configure(profile);

    REQUIRE(arena.prepare(1024));
    CHECK(arena.allocate_array<float>(64) != nullptr);
    CHECK(arena.used() == 256u);
    arena.end_stamp();
    const std::size_t first_growths = arena.growths();

    // A stroke of similar stamps allocates on its first stamp and never again.
    for (int i = 0; i < 8; ++i) {
        REQUIRE(arena.prepare(1024));
        CHECK(arena.allocate_array<float>(64) != nullptr);
        arena.end_stamp();
    }
    CHECK(arena.growths() == first_growths);

    // PAST THE HARD BOUND the work is processed in blocks rather than
    // allocated, which is what stops a big footprint on a constrained profile
    // from becoming the peak that kills the app.
    CHECK_FALSE(arena.prepare(1u << 20));
    CHECK(arena.capacity() <= profile.scratch_budget);
    CHECK(arena.block_elements(sizeof(float), 1u << 20) == 1024u);
    CHECK(arena.block_count(sizeof(float), 4096) == 4u);
    CHECK(arena.high_water() <= profile.scratch_budget);
}

TEST_CASE("memory: a pin turns a trim into a report of what it would have released") {
    const Mesh cage = grid_quads(4, 1.0f);
    auto surface = MultiresSurface::from_mesh(cage);
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    surface->positions_at(surface->max_level());

    const std::uint64_t checksum = surface->detail_checksum();
    const std::size_t authoritative = surface->memory().authoritative;

    memory::TrimGate gate;
    {
        memory::MemoryPin pin(gate);
        const memory::TrimReport report =
            mesh::trim_surface(*surface, memory::Pressure::Critical, &gate);
        CHECK(report.pinned);
        CHECK(report.total_released > 0);
        // NOTHING WAS ACTUALLY RELEASED. A memory warning arriving mid-save
        // gets an honest answer instead of a document mutating under the
        // writer.
        CHECK(surface->memory().rebuildable > 0);
    }

    const memory::TrimReport done =
        mesh::trim_surface(*surface, memory::Pressure::Critical, &gate);
    CHECK_FALSE(done.pinned);
    CHECK(done.total_released > 0);
    CHECK(surface->memory().rebuildable == 0);
    // THE WORK IS UNTOUCHED, by checksum rather than by inspection.
    CHECK(surface->detail_checksum() == checksum);
    CHECK(surface->memory().authoritative == authoritative);
}

TEST_CASE("memory: a constrained profile keeps the active levels and no others") {
    const Mesh cage = grid_quads(4, 1.0f);
    auto surface = MultiresSurface::from_mesh(cage);
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    surface->positions_at(surface->max_level());

    const std::uint64_t checksum = surface->detail_checksum();
    std::uint32_t resident_before = 0;
    for (std::uint32_t l = 0; l < surface->level_count(); ++l)
        if (surface->level_resident(l)) ++resident_before;
    REQUIRE(resident_before > 2);

    // THE PROFILE IS FILLED BY THE HOST, and a desktop test is exactly where
    // constrained behaviour has to be observable — the portable core detects no
    // device, so there is nothing else to exercise it.
    memory::SculptMemoryProfile profile;
    profile.memory_class = memory::MemoryClass::Constrained;
    surface->set_memory_profile(profile);
    surface->set_display_level(surface->max_level());

    std::uint32_t resident_after = 0;
    for (std::uint32_t l = 0; l < surface->level_count(); ++l)
        if (surface->level_resident(l)) ++resident_after;
    CHECK(resident_after < resident_before);
    CHECK(surface->level_resident(surface->display_level()));
    // The levels that went kept their authoritative detail: residency is a
    // cache decision and never a content one.
    CHECK(surface->detail_checksum() == checksum);

    // And the surface reconstructs identically from what is left.
    const std::vector<kernel::cfloat3> rebuilt = surface->positions_at(surface->max_level());
    surface->drop_all_caches();
    const std::vector<kernel::cfloat3>& again = surface->positions_at(surface->max_level());
    REQUIRE(again.size() == rebuilt.size());
    for (std::size_t i = 0; i < again.size(); ++i) {
        // BIT-IDENTICAL, not close. The only honest account of "this was a
        // cache" is that rebuilding it produced the same bytes.
        CHECK(again[i].x == rebuilt[i].x);
        CHECK(again[i].y == rebuilt[i].y);
        CHECK(again[i].z == rebuilt[i].z);
    }
}

TEST_CASE("maintenance: the queue coalesces, refuses mid-stroke, and honours a budget") {
    mesh::MaintenanceQueue queue;
    queue.request(mesh::MaintenanceKind::IndexRebuild, 0, 100);
    queue.request(mesh::MaintenanceKind::IndexRebuild, 0, 250);
    queue.request(mesh::MaintenanceKind::ChunkCompaction, 0);
    REQUIRE(queue.size() == 2);
    CHECK(queue.items()[0].requests == 2u);
    CHECK(queue.items()[0].estimated_micros == 250u);

    // A POINTER EVENT IS NOT A MAINTENANCE WINDOW.
    queue.begin_stroke();
    CHECK(queue.service(1000, [](const mesh::MaintenanceItem&) { return true; }) == 0u);
    CHECK(queue.size() == 2);
    queue.end_stroke();

    // A budget of zero is "one item, whatever it costs".
    CHECK(queue.service(0, [](const mesh::MaintenanceItem&) { return true; }) == 1u);
    CHECK(queue.size() == 1);
    // An item the host declines stays queued, in place.
    CHECK(queue.service(1000, [](const mesh::MaintenanceItem&) { return false; }) == 0u);
    CHECK(queue.size() == 1);
}

TEST_CASE("preflight: an operation reports the peak that exceeds its result") {
    const Mesh mesh = grid_triangles(16, 1.0f);
    const mesh::SurfacePreflight estimate = mesh::preflight_to_dynamic(mesh);
    CHECK(estimate.allowed);
    CHECK(estimate.persistent_bytes > 0);
    // The peak is the whole reason this is asked: the conversion holds the weld
    // map beside the half-edge structure it is building.
    CHECK(estimate.peak_bytes > estimate.persistent_bytes);

    // A budget below the PEAK refuses, even when the result would have fitted.
    const mesh::SurfacePreflight refused =
        mesh::preflight_to_dynamic(mesh, estimate.persistent_bytes);
    CHECK_FALSE(refused.allowed);
    CHECK(refused.error == memory::BudgetError::OverBudget);
}
