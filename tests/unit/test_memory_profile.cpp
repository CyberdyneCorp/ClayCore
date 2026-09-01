// WHAT A HOST DECLARES, AND WHAT IT MAY NEVER CHANGE (sculpt-runtime and
// scene-model specs, add-extreme-poly-runtime 1.3, 4.1 to 4.3, 4.7).
//
// The profile is the one place a host's device gets to speak to the runtime,
// and the whole design of it rests on a single claim: EVERY FIELD IS A HINT.
// Each one names something that can be recomputed exactly from what was
// committed — normals during a drag, index quality, display level, cache
// residency, the rate a preview drains — and there is deliberately no field
// that reaches the committed result. A deferred SPLIT would make the mesh a
// function of machine speed, and this tree spends real effort on determinism
// that a budget-dependent topology would throw away.
//
// That claim is not testable by reading the struct, because the failure it
// guards against is a future field. So it is tested as the spec states it: the
// same stroke, once under a full profile and once under a constrained one, has
// to produce BYTE-IDENTICAL committed geometry. A profile that had started to
// mean something for the result would fail that and nothing else.
//
// THE LEDGER IS THE OTHER HALF. A single total is not what a memory warning
// asks for: a host needs to know WHICH PART, because that decides what it is
// allowed to release. So the three roll-ups have to be a partition of the
// categories — every category in exactly one of them, and the three summing to
// the total. A category added without being classified is the defect, and it
// arrives as a number that quietly stops adding up rather than as an error.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "clay/io/memory.h"
#include "clay/memory/budget.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using memory::MemoryCategory;
using memory::MemoryClass;
using memory::MemoryLedger;
using memory::Pressure;
using memory::SculptMemoryProfile;
using mesh::Mesh;
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

// The same stroke, twice, under whatever profile the caller declares. Returns
// the committed geometry — the coefficients, not an evaluation of them, because
// the coefficients are what the document holds.
std::vector<cfloat3> stroke_under(const SculptMemoryProfile& profile, std::uint64_t* checksum) {
    auto surface = MultiresSurface::from_mesh(quad_field(6, 0.25f));
    REQUIRE(surface.has_value());
    for (int i = 0; i < 3; ++i) REQUIRE(surface->add_level());
    surface->set_memory_profile(profile);
    REQUIRE(surface->set_sculpt_level(surface->max_level()));
    REQUIRE(surface->set_display_level(surface->max_level()));

    mesh::MultiresSculptor sculptor(*surface);
    sculptor.set_defer_normals(profile.defer_normals_in_stroke);
    mesh::MeshBrushSettings brush;
    brush.radius = 0.3f;
    brush.strength = 0.35f;
    brush.smooth_iterations = 2;
    sculptor.begin_stroke();
    const mesh::MeshBrush verbs[4] = {mesh::MeshBrush::Draw, mesh::MeshBrush::Inflate,
                                      mesh::MeshBrush::Clay, mesh::MeshBrush::Smooth};
    for (int i = 0; i < 12; ++i) {
        brush.center = cf3(-0.3f + 0.06f * static_cast<float>(i), 0.0f,
                           0.1f * static_cast<float>(i % 3));
        brush.geodesic = mesh::default_geodesic(verbs[i % 4]);
        sculptor.stamp(verbs[i % 4], brush);
    }
    sculptor.flush_normals();
    *checksum = surface->detail_checksum();
    return surface->positions_at(surface->max_level());
}

}  // namespace

TEST_CASE("memory profile: the three roll-ups partition the categories") {
    std::size_t counted = 0;
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i) {
        const auto category = static_cast<MemoryCategory>(i);
        CAPTURE(i);
        const char* name = memory::memory_category_name(category);
        REQUIRE(name != nullptr);
        CHECK(std::string(name).size() > 0);
        // EXACTLY ONE of the three. A category in none of them is bytes that
        // stop adding up; a category in two is bytes counted twice, and the
        // second sighting is what tells a host it may release the user's work.
        const int classes = (memory::category_is_essential(category) ? 1 : 0) +
                            (memory::category_is_rebuildable(category) ? 1 : 0) +
                            (memory::category_is_undoable(category) ? 1 : 0);
        CHECK(classes == 1);
        ++counted;
    }
    CHECK(counted == memory::kMemoryCategoryCount);

    // The names are distinct, so a host keying a dictionary by them does not
    // silently lose a line.
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i)
        for (std::size_t j = i + 1; j < memory::kMemoryCategoryCount; ++j)
            CHECK(std::string(memory::memory_category_name(static_cast<MemoryCategory>(i))) !=
                  std::string(memory::memory_category_name(static_cast<MemoryCategory>(j))));
}

TEST_CASE("memory profile: a ledger sums, merges and rolls up") {
    MemoryLedger a;
    a.add(MemoryCategory::BaseGeometry, 100);
    a.add(MemoryCategory::MultiresDetail, 200);
    a.add(MemoryCategory::ChunkIndex, 30);
    a.add(MemoryCategory::UndoHistory, 7);
    CHECK(a.of(MemoryCategory::BaseGeometry) == 100u);
    CHECK(a.essential() == 300u);
    CHECK(a.rebuildable() == 30u);
    CHECK(a.undoable() == 7u);
    CHECK(a.total() == 337u);
    CHECK(a.essential() + a.rebuildable() + a.undoable() == a.total());

    // ADDING IS ACCUMULATION, not assignment: two subsystems answering for the
    // same category both count.
    a.add(MemoryCategory::BaseGeometry, 50);
    CHECK(a.of(MemoryCategory::BaseGeometry) == 150u);

    MemoryLedger b;
    b.add(MemoryCategory::Scratch, 11);
    b.add(MemoryCategory::BaseGeometry, 1);
    a.merge(b);
    CHECK(a.of(MemoryCategory::BaseGeometry) == 151u);
    CHECK(a.of(MemoryCategory::Scratch) == 11u);
    CHECK(a.essential() + a.rebuildable() + a.undoable() == a.total());

    a.clear();
    CHECK(a.total() == 0u);
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i) CHECK(a.bytes[i] == 0u);
}

TEST_CASE("memory profile: a hierarchy answers for itself in the shared vocabulary") {
    auto surface = MultiresSurface::from_mesh(quad_field(6, 0.25f));
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    surface->positions_at(surface->max_level());
    surface->chunks_at(surface->max_level());

    MemoryLedger ledger;
    mesh::report_surface_memory(*surface, &ledger);
    const mesh::MultiresMemory direct = surface->memory();

    // THE TWO SHAPES AGREE. `MultiresMemory` is what the hierarchy has always
    // reported and the ledger is the vocabulary a host holding three
    // representations reads; a divergence here means a host's totals depend on
    // which call it made.
    CHECK(ledger.of(MemoryCategory::BaseGeometry) == direct.base);
    CHECK(ledger.of(MemoryCategory::Topology) == direct.topology);
    CHECK(ledger.of(MemoryCategory::MultiresDetail) == direct.detail);
    CHECK(ledger.of(MemoryCategory::ChunkIndex) == direct.chunk_index);
    CHECK(ledger.essential() == direct.authoritative);
    CHECK(ledger.rebuildable() == direct.rebuildable);
    CHECK(ledger.total() == direct.total);
    CHECK(direct.chunk_index > 0u);
}

TEST_CASE("memory report: the surface tier is added in, and the fields still sum") {
    io::ClaySpaceDoc doc;
    const io::MemoryReport bare = io::document_memory(doc, nullptr);
    CHECK(bare.surface_content == 0u);
    CHECK(bare.multires_detail == 0u);
    // THE INVARIANT THE WHOLE REPORT RESTS ON: the fields sum to the total. A
    // category added without being added to the sum is the defect this exists
    // to catch, and it has to fail here rather than on review.
    CHECK(bare.essential() + bare.rebuildable() + bare.undoable() == bare.total);

    MemoryLedger surfaces;
    surfaces.add(MemoryCategory::BaseGeometry, 1000);
    surfaces.add(MemoryCategory::Topology, 400);
    surfaces.add(MemoryCategory::MultiresDetail, 2000);
    surfaces.add(MemoryCategory::SculptLayers, 300);
    surfaces.add(MemoryCategory::ChunkIndex, 60);
    surfaces.add(MemoryCategory::EvaluatedCache, 90);
    surfaces.add(MemoryCategory::Scratch, 20);
    surfaces.add(MemoryCategory::UndoHistory, 5);

    const io::MemoryReport with = io::document_memory(doc, nullptr, &surfaces);
    CHECK(with.surface_content == 1400u);  // base + topology + masks
    CHECK(with.multires_detail == 2000u);
    CHECK(with.sculpt_layers == 300u);
    CHECK(with.surface_caches == 150u);
    CHECK(with.surface_scratch == 20u);
    CHECK(with.surface_undo == 5u);
    CHECK(with.total == bare.total + surfaces.total());
    CHECK(with.essential() + with.rebuildable() + with.undoable() == with.total);
    // A HOST CAN ACT ON THE THREE. The wrinkles are not releasable and the
    // chunk index is, and the report says which is which without the host
    // having to know what a chunk index is.
    CHECK(with.essential() >= 2000u);
    CHECK(with.rebuildable() >= 170u);
    CHECK(with.undoable() >= 5u);

    // NULL IS A DOCUMENT WITH NO HOST-HELD SURFACES, and reports zero for them
    // rather than guessing — the C ABI says a hierarchy is opaque and owning,
    // so a document cannot walk one.
    CHECK(io::document_memory(doc, nullptr, nullptr).total == bare.total);
}

TEST_CASE("memory profile: a constrained profile keeps the active levels and nothing else") {
    auto surface = MultiresSurface::from_mesh(quad_field(6, 0.25f));
    REQUIRE(surface.has_value());
    for (int i = 0; i < 3; ++i) REQUIRE(surface->add_level());
    for (std::uint32_t l = 0; l < surface->level_count(); ++l) surface->positions_at(l);
    REQUIRE(surface->memory().resident_levels == surface->level_count());

    SculptMemoryProfile profile;
    profile.memory_class = MemoryClass::Constrained;
    profile.max_resident_levels = 2;
    surface->set_memory_profile(profile);
    CHECK(surface->memory_profile().memory_class == MemoryClass::Constrained);
    CHECK(surface->memory_profile().constrained());

    // Applied AT THE MOMENT THE HOST CAUSED, which is this call and the two
    // level setters and nowhere else. An engine evicting on its own high-water
    // mark would be a second invalidation source a host cannot predict.
    CHECK(surface->memory().resident_levels < surface->level_count());
    CHECK(surface->level_resident(surface->sculpt_level()));
    CHECK(surface->level_resident(surface->display_level()));
    // The authoritative detail of every level is still there — residency is
    // about caches.
    const std::uint64_t checksum = surface->detail_checksum();
    for (std::uint32_t l = 0; l < surface->level_count(); ++l) {
        CAPTURE(l);
        CHECK(surface->positions_at(l).size() > 0u);
    }
    CHECK(surface->detail_checksum() == checksum);

    // A FULL profile imposes nothing, which is what every existing caller gets
    // and what makes this library behave as it did before profiles existed.
    SculptMemoryProfile full;
    CHECK_FALSE(full.constrained());
    CHECK(full.cache_budget == 0u);
    CHECK(full.scratch_budget == 0u);
    CHECK(full.max_resident_levels == 0u);
    CHECK_FALSE(full.defer_normals_in_stroke);
    CHECK(full.allow_index_rebuild);
}

TEST_CASE("memory profile: the same stroke commits the same bytes under any profile") {
    // THE 1.3 GATE. Every field of the profile is a hint, and the way that is
    // enforced is the SHAPE OF THE TYPE — there is no field a host can set that
    // reaches a contract row. This is the assertion that would fail if one were
    // ever added: a memory-saving mode that changed the sculpt.
    SculptMemoryProfile full;

    SculptMemoryProfile constrained;
    constrained.memory_class = MemoryClass::Constrained;
    constrained.max_resident_levels = 1;
    constrained.cache_budget = 64u * 1024u;
    constrained.scratch_budget = 32u * 1024u;
    constrained.preview_budget = 16u * 1024u;
    constrained.preview_chunks_per_frame = 2;
    constrained.defer_normals_in_stroke = true;
    constrained.allow_index_rebuild = false;

    SculptMemoryProfile minimal = constrained;
    minimal.memory_class = MemoryClass::Minimal;
    minimal.max_resident_levels = 1;

    std::uint64_t checksum_full = 0, checksum_constrained = 0, checksum_minimal = 0;
    const std::vector<cfloat3> a = stroke_under(full, &checksum_full);
    const std::vector<cfloat3> b = stroke_under(constrained, &checksum_constrained);
    const std::vector<cfloat3> c = stroke_under(minimal, &checksum_minimal);

    REQUIRE_FALSE(a.empty());
    // The coefficients themselves, which is what the document holds.
    CHECK(checksum_constrained == checksum_full);
    CHECK(checksum_minimal == checksum_full);
    // And the surface they reconstruct, bit for bit. `defer_normals_in_stroke`
    // is set on the constrained runs, so this is also the assertion that a
    // deferred normal flush ends EXACTLY where a per-stamp one does.
    REQUIRE(b.size() == a.size());
    REQUIRE(c.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(b[i].x == a[i].x);
        CHECK(b[i].y == a[i].y);
        CHECK(b[i].z == a[i].z);
        CHECK(c[i].x == a[i].x);
        CHECK(c[i].y == a[i].y);
        CHECK(c[i].z == a[i].z);
    }
}

TEST_CASE("memory profile: the vocabulary crosses as text a host can show a person") {
    CHECK(std::string(memory::memory_class_name(MemoryClass::Full)) != "");
    CHECK(std::string(memory::memory_class_name(MemoryClass::Constrained)) !=
          std::string(memory::memory_class_name(MemoryClass::Minimal)));
    const Pressure levels[4] = {Pressure::None, Pressure::Warning, Pressure::Urgent,
                                Pressure::Critical};
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            CHECK(std::string(memory::pressure_name(levels[i])) !=
                  std::string(memory::pressure_name(levels[j])));
}
