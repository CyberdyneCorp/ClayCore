// THE MEMORY-PRESSURE GATE (sculpt-runtime spec, add-extreme-poly-runtime 4.6
// and 7.6).
//
// `trim(pressure)` has exactly two ways to be wrong and they fail differently:
//
//   1. IT TOUCHED THE WORK. The user's authoritative content — the cage, a
//      level's topology, the detail coefficients, a sculpt layer, a mask — is
//      gone or changed. This is the serious one, because a host calls `trim`
//      in response to an operating-system warning, which is to say at the
//      worst possible moment and without asking anybody. The assertion is a
//      CHECKSUM before and after, not a reading of the eviction code.
//
//   2. A DROPPED CACHE DOES NOT COME BACK THE SAME. Something that
//      reconstructs to a DIFFERENT surface was never a cache; it was
//      authoritative content that happened to be cheap to recompute until it
//      was not. The assertion is the surface itself, bit for bit, before the
//      trim and after the rebuild — and it has to be taken over positions that
//      were EVALUATED before the trim, or the comparison is between two
//      evaluations of the same thing and proves nothing about the release.
//
// AND THE PIN, which is the third failure and the one with no symptom at all:
// a memory warning arriving mid-save releases something the writer is halfway
// through reading, and the document that lands on disk is the one nobody
// wrote. A held `MemoryPin` makes `trim` a no-op that REPORTS what it would
// have released, so the host gets an answer rather than a mutation.
//
// The eviction ORDER is asserted as a monotone chain rather than as a list of
// names: warning releases at most what urgent does, and urgent at most what
// critical does. A chain is what a host reasons about; the names are in the
// spec and in `surface_view.h` and would be re-stated here as prose either way.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/memory/budget.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/surface_frame.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::Mesh;
using mesh::MultiresMemory;
using mesh::MultiresSurface;

namespace {

Mesh quad_field(int n, float spacing) {
    Mesh m;
    const float half = spacing * static_cast<float>(n) * 0.5f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + spacing * static_cast<float>(x), 0.0f,
                                      -half + spacing * static_cast<float>(z)));
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.quads.push_back(at(x, z));
            m.quads.push_back(at(x + 1, z));
            m.quads.push_back(at(x + 1, z + 1));
            m.quads.push_back(at(x, z + 1));
            m.indices.insert(m.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1), at(x, z),
                                               at(x + 1, z + 1), at(x, z + 1)});
        }
    return m;
}

// A hierarchy with real detail in it. A trim over a hierarchy whose levels
// carry nothing but pure subdivision would reconstruct identically even if the
// coefficients HAD been dropped, which is the one thing this file is about.
std::optional<MultiresSurface> sculpted_hierarchy(int n = 6, std::uint32_t levels = 3) {
    auto surface = MultiresSurface::from_mesh(quad_field(n, 0.25f));
    if (!surface.has_value()) return std::nullopt;
    for (std::uint32_t i = 0; i < levels; ++i)
        if (!surface->add_level()) return std::nullopt;

    const std::uint32_t level = surface->max_level();
    const std::vector<cfloat3>& positions = surface->positions_at(level);
    std::vector<std::uint32_t> moved;
    for (std::uint32_t i = 0; i < positions.size(); ++i) {
        const cfloat3 p = positions[i];
        // A ridge, so the detail is a function of position rather than a
        // constant a bug could reproduce by accident.
        if (clength(p - cf3(0, 0, 0)) < 0.45f) moved.push_back(i);
    }
    Mesh& level_mesh = surface->level_mesh(level);
    for (std::uint32_t v : moved)
        level_mesh.positions[v].y += 0.05f + 0.02f * static_cast<float>(v % 7);
    surface->absorb_level_edit(level, moved);
    surface->positions_at(level);
    return surface;
}

// Everything a host would call rebuildable, evaluated, so that a trim has
// something to release and the comparison afterwards is against a real state.
void warm_every_level(MultiresSurface& surface) {
    for (std::uint32_t l = 0; l < surface.level_count(); ++l) {
        surface.positions_at(l);
        surface.normals_at(l);
        surface.frames_at(l);
        surface.chunks_at(l);
    }
}

bool identical(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        // BIT equality, not a tolerance. "It reconstructs to an identical
        // surface" is the requirement's own word, and a tolerance here would
        // pass a rebuild that quietly re-ordered a float sum.
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("memory gate: a critical trim keeps the checksum and every dropped cache comes back") {
    auto surface = sculpted_hierarchy();
    REQUIRE(surface.has_value());
    warm_every_level(*surface);

    const std::uint64_t checksum = surface->detail_checksum();
    const MultiresMemory before = surface->memory();
    REQUIRE(before.rebuildable > 0);
    REQUIRE(before.authoritative > 0);

    // Copies, taken while the caches are warm, of what the caches produce.
    std::vector<std::vector<cfloat3>> positions(surface->level_count());
    std::vector<std::vector<cfloat3>> normals(surface->level_count());
    for (std::uint32_t l = 0; l < surface->level_count(); ++l) {
        positions[l] = surface->positions_at(l);
        normals[l] = surface->normals_at(l);
    }
    const Mesh base_before = surface->base_mesh();

    const memory::TrimReport report =
        mesh::trim_surface(*surface, memory::Pressure::Critical);
    CHECK_FALSE(report.pinned);
    CHECK(report.total_released > 0);
    // A trim reports what it released BY CATEGORY, and every category it names
    // has to be a rebuildable one. A release attributed to `MultiresDetail` or
    // `BaseGeometry` is the serious failure, arriving as a number rather than
    // as a crash.
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i) {
        const auto category = static_cast<memory::MemoryCategory>(i);
        CAPTURE(memory::memory_category_name(category));
        if (report.released[i] > 0) CHECK(memory::category_is_rebuildable(category));
    }

    // 1. THE WORK IS UNTOUCHED.
    CHECK(surface->detail_checksum() == checksum);
    const MultiresMemory after = surface->memory();
    CHECK(after.authoritative == before.authoritative);
    CHECK(after.base == before.base);
    CHECK(after.topology == before.topology);
    CHECK(after.detail == before.detail);
    CHECK(after.rebuildable < before.rebuildable);
    // The cage itself, position by position.
    const Mesh base_after = surface->base_mesh();
    REQUIRE(base_after.positions.size() == base_before.positions.size());
    CHECK(identical(base_after.positions, base_before.positions));
    CHECK(base_after.indices == base_before.indices);

    // 2. EVERY DROPPED CACHE RECONSTRUCTS IDENTICALLY.
    for (std::uint32_t l = 0; l < surface->level_count(); ++l) {
        CAPTURE(l);
        CHECK(identical(surface->positions_at(l), positions[l]));
        CHECK(identical(surface->normals_at(l), normals[l]));
    }
}

TEST_CASE("memory gate: the chunk partition a trim released comes back naming the same faces") {
    // A partition is rebuildable, and the requirement's word is IDENTICAL. A
    // partition that reconstructed a correct surface under DIFFERENT chunk ids
    // would satisfy the surface comparison above and still break every host
    // that had already uploaded by those ids.
    auto surface = sculpted_hierarchy();
    REQUIRE(surface.has_value());
    const std::uint32_t level = surface->max_level();

    std::vector<std::vector<std::uint32_t>> before;
    {
        const mesh::ChunkTable& table = surface->chunks_at(level);
        REQUIRE(table.live_count() > 1);
        for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
            std::vector<std::uint32_t> faces;
            if (const mesh::SurfaceChunk* c = table.chunk(i))
                for (mesh::FaceId f : c->faces) faces.push_back(f.slot);
            before.push_back(faces);
        }
    }

    mesh::trim_surface(*surface, memory::Pressure::Critical);

    const mesh::ChunkTable& table = surface->chunks_at(level);
    REQUIRE(table.slot_count() == before.size());
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        CAPTURE(i);
        std::vector<std::uint32_t> faces;
        if (const mesh::SurfaceChunk* c = table.chunk(i))
            for (mesh::FaceId f : c->faces) faces.push_back(f.slot);
        CHECK(faces == before[i]);
    }
}

TEST_CASE("memory gate: the eviction order is a chain, and no pressure releases the work") {
    const memory::Pressure order[4] = {memory::Pressure::None, memory::Pressure::Warning,
                                       memory::Pressure::Urgent, memory::Pressure::Critical};
    std::size_t released[4] = {};
    for (int i = 0; i < 4; ++i) {
        auto surface = sculpted_hierarchy();
        REQUIRE(surface.has_value());
        warm_every_level(*surface);
        // The artist is at the finest level and looking at it, which is the
        // residency the eviction order is stated against.
        REQUIRE(surface->set_sculpt_level(surface->max_level()));
        REQUIRE(surface->set_display_level(surface->max_level()));
        warm_every_level(*surface);

        const std::uint64_t checksum = surface->detail_checksum();
        const MultiresMemory before = surface->memory();
        const memory::TrimReport report = mesh::trim_surface(*surface, order[i]);
        const MultiresMemory after = surface->memory();

        CAPTURE(memory::pressure_name(order[i]));
        released[i] = report.total_released;
        CHECK(after.authoritative == before.authoritative);
        CHECK(surface->detail_checksum() == checksum);
        CHECK(report.pressure == order[i]);
    }
    // NONE RELEASES NOTHING: releasing a cache at no pressure is the engine
    // deciding for the host, which is exactly what the design refuses.
    CHECK(released[0] == 0);
    CHECK(released[1] <= released[2]);
    CHECK(released[2] <= released[3]);
    CHECK(released[3] > released[1]);
}

TEST_CASE("memory gate: a pin makes a trim a report rather than a mutation") {
    auto surface = sculpted_hierarchy();
    REQUIRE(surface.has_value());
    warm_every_level(*surface);
    const MultiresMemory before = surface->memory();
    REQUIRE(before.rebuildable > 0);

    memory::TrimGate gate;
    {
        // A SAVE IS RUNNING. Whatever the operating system says next, the
        // document under the writer does not move.
        memory::MemoryPin pin(gate);
        CHECK(gate.pinned());
        {
            // Reentrant: a readback inside the save must not un-pin the save
            // when it returns. This is the shape an early return — a cancelled
            // save — actually has.
            memory::MemoryPin inner(gate);
            CHECK(gate.pins() == 2u);
        }
        CHECK(gate.pinned());

        const memory::TrimReport report =
            mesh::trim_surface(*surface, memory::Pressure::Critical, &gate);
        CHECK(report.pinned);
        // It still ANSWERS. A host that gets zero back cannot tell "nothing to
        // release" from "I am not allowed to", and those call for different
        // decisions.
        CHECK(report.total_released > 0);
        CHECK(surface->memory().rebuildable == before.rebuildable);
    }
    CHECK_FALSE(gate.pinned());

    // And once the save is done the same call releases for real.
    const memory::TrimReport after_save =
        mesh::trim_surface(*surface, memory::Pressure::Critical, &gate);
    CHECK_FALSE(after_save.pinned);
    CHECK(after_save.total_released > 0);
    CHECK(surface->memory().rebuildable < before.rebuildable);
}

TEST_CASE("memory gate: an adaptive surface's trim gives back slack and keeps the surface") {
    auto surface = DynamicSurface::from_mesh(quad_field(12, 0.2f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);

    // Splits and merges are what leave slack in the face arena, so the trim
    // has to have something to give back before it is asked.
    mesh::MeshBrushSettings brush;
    brush.radius = 0.3f;
    brush.strength = 0.5f;
    mesh::DynamicTopologySettings topology;
    topology.enabled = true;
    topology.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    for (int i = 0; i < 6; ++i) {
        brush.center = cf3(-0.3f + 0.12f * static_cast<float>(i), 0.0f, 0.0f);
        sculptor.stamp(mesh::MeshBrush::Draw, brush, topology);
    }

    mesh::DynamicSurfaceExportOptions options;
    options.normals = false;
    options.colors = false;
    options.uvs = false;
    const Mesh before = surface->to_mesh(options);
    const std::size_t faces_before = surface->stats().faces;

    memory::TrimGate gate;
    {
        memory::MemoryPin pin(gate);
        const memory::TrimReport pinned =
            mesh::trim_surface(sculptor, memory::Pressure::Critical, &gate);
        CHECK(pinned.pinned);
        CHECK(sculptor.bvh().chunks().arena_slack() > 0);
    }

    const std::size_t index_bytes = sculptor.bvh().chunks().bytes();
    const memory::TrimReport report =
        mesh::trim_surface(sculptor, memory::Pressure::Critical);
    CHECK_FALSE(report.pinned);
    CHECK(sculptor.bvh().chunks().arena_slack() == 0u);
    // AND THE MEMORY IS ACTUALLY GONE, rather than a subtraction of two
    // figures that both describe storage the table is still holding.
    CHECK(report.total_released > 0u);
    CHECK(report.released[static_cast<std::size_t>(memory::MemoryCategory::ChunkIndex)] > 0u);
    CHECK(sculptor.bvh().chunks().bytes() < index_bytes);

    // THE SURFACE IS THE SAME SURFACE. A compaction moves every span in the
    // arena; if it moved a face between chunks or lost one, this is where it
    // shows.
    const Mesh after = surface->to_mesh(options);
    CHECK(surface->stats().faces == faces_before);
    REQUIRE(after.positions.size() == before.positions.size());
    CHECK(identical(after.positions, before.positions));
    CHECK(after.indices == before.indices);

    // And every face is still in exactly one chunk.
    std::vector<int> seen;
    const mesh::ChunkTable& table = sculptor.bvh().chunks();
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        const mesh::SurfaceChunk* c = table.chunk(i);
        if (c == nullptr) continue;
        for (mesh::FaceId f : c->faces) {
            if (f.slot >= seen.size()) seen.resize(f.slot + 1, 0);
            ++seen[f.slot];
        }
    }
    for (int n : seen) CHECK(n <= 1);
}

TEST_CASE("regression: a sculpted level's display normals are the hierarchy's, warm or cold") {
    // THE BUG THIS PINS (found by the 4.6 gate above, and older than this
    // change). Two things wrote `LevelCache::mesh.normals` and they disagreed:
    // `MeshSculptor`, which the hierarchy calls to do the deformation, derives
    // a normal from the level mesh's TRIANGLES; everything in
    // `multires_eval.cpp` derives it from the level's own faces by Newell,
    // which on a subdivision quad is a different vector. `absorb_level_edit`
    // then rewrote the positions those normals had been computed from, reading
    // the coefficients back through the frame, and marked nothing.
    //
    // So a sculpted hierarchy shaded one way while its cache was warm and
    // another way after any rebuild — measured at 497 of 2401 vertices and up
    // to 0.02 in the unit normal, about a degree, over the sculpted region.
    // The failure had no edit behind it and nothing in the surface to explain
    // it: a host under memory pressure watched the shading of the region the
    // artist had just worked on move. And it is exactly what makes "every
    // dropped cache reconstructs to an identical surface" false.
    //
    // Asserted WITHOUT a trim as well, because the warm state was wrong on its
    // own terms — the trim is only what made it visible.
    auto surface = MultiresSurface::from_mesh(quad_field(6, 0.25f));
    REQUIRE(surface.has_value());
    for (int i = 0; i < 3; ++i) REQUIRE(surface->add_level());
    const std::uint32_t level = surface->max_level();
    REQUIRE(surface->set_sculpt_level(level));
    REQUIRE(surface->set_display_level(level));

    mesh::MultiresSculptor sculptor(*surface);
    mesh::MeshBrushSettings brush;
    brush.center = cf3(0, 0, 0);
    brush.radius = 0.35f;
    brush.strength = 0.5f;
    sculptor.begin_stroke();
    REQUIRE(sculptor.stamp(mesh::MeshBrush::Draw, brush) > 0);

    // 1. THE WARM STATE IS WHAT THE HIERARCHY'S OWN DEFINITION SAYS.
    const std::vector<cfloat3> warm = surface->normals_at(level);
    std::vector<cfloat3> expected;
    mesh::level_normals(surface->topology_at(level), surface->connectivity_at(level),
                        surface->positions_at(level), &expected);
    REQUIRE(warm.size() == expected.size());
    CHECK(identical(warm, expected));

    // 2. AND A REBUILD PRODUCES THE SAME BYTES.
    surface->drop_all_caches();
    CHECK(identical(surface->normals_at(level), warm));

    // 3. AND THE FIX DID NOT COST THE SHORT CIRCUIT. A second stamp at the
    //    same level, with the levels below it released, must not walk from the
    //    cage — which is what draining the pending normals through the walk
    //    instead of directly would have made it do. `test_multires_dirty.cpp`
    //    owns this property; it is re-asserted here because this file is what
    //    would have broken it.
    surface->positions_at(level);
    surface->drop_intermediate_caches();
    REQUIRE(surface->memory().resident_levels == 1u);
    surface->reset_eval_stats();
    brush.center = cf3(0.2f, 0.0f, 0.1f);
    REQUIRE(sculptor.stamp(mesh::MeshBrush::Draw, brush) > 0);
    surface->positions_at(level);
    CHECK(surface->eval_stats().full_level_rebuilds == 0u);
    CHECK(surface->memory().resident_levels == 1u);
}
