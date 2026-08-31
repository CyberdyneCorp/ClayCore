// Propagation is LOCAL (mesh-multires spec, add-mesh-multires).
//
// The claim is easy to make and easy to get wrong in a way nothing notices: a
// dab at a coarse level of a deep hierarchy must reconstruct the descendants of
// what it touched and nothing else. An implementation that quietly rebuilds a
// whole level is correct and unusable — the surface is right and the pointer
// lags — so this file asserts the two halves separately:
//
//   1. UNRELATED PATCHES ARE BYTE-IDENTICAL. Not close. Identical, because the
//      only honest account of "this dab did not reach there" is that nothing
//      there was written at all.
//   2. THE COST IS MEASURED against a cold full reconstruction, through
//      `eval_stats`, rather than asserted in a comment.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/multires_sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresError;
using mesh::MultiresSculptor;
using mesh::MultiresSurface;

namespace {

Mesh plane_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

MultiresSurface build(const Mesh& m, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(m, {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

// Which level-`level` vertices belong to base patch `patch`.
std::vector<char> vertices_of_patch(MultiresSurface& s, std::uint32_t level, std::uint32_t patch) {
    const mesh::LevelTopology& t = s.topology_at(level);
    std::vector<char> mark(t.vertex_count, 0);
    for (std::uint32_t f = 0; f < t.face_count; ++f) {
        if (t.patch_of(f) != patch) continue;
        std::uint32_t arity = 0;
        const std::uint32_t* c = t.face(f, &arity);
        for (std::uint32_t k = 0; k < arity; ++k) mark[c[k]] = 1;
    }
    return mark;
}

}  // namespace

TEST_CASE("a dab leaves unrelated base patches byte-identical") {
    MultiresSurface s = build(plane_quads(6, 3.0f), 3);
    const std::uint32_t fine = 3;
    REQUIRE(s.set_display_level(fine));
    const std::vector<cfloat3> before = s.positions_at(fine);

    // The patch under the corner of the sheet, far from the middle.
    const std::uint32_t far_patch = 0;
    const std::vector<char> far_mark = vertices_of_patch(s, fine, far_patch);
    std::size_t far_count = 0;
    for (char m : far_mark) far_count += m ? 1u : 0u;
    REQUIRE(far_count > 0);

    // A dab in the MIDDLE of the sheet, at a coarse level.
    REQUIRE(s.set_sculpt_level(1));
    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0.0f, 0.0f, 0.0f);
    settings.radius = 0.8f;
    settings.strength = 0.5f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);

    const std::vector<cfloat3>& after = s.positions_at(fine);
    REQUIRE(after.size() == before.size());

    std::size_t moved_far = 0, moved_total = 0;
    for (std::size_t v = 0; v < after.size(); ++v) {
        const bool same = after[v].x == before[v].x && after[v].y == before[v].y &&
                          after[v].z == before[v].z;
        if (!same) {
            ++moved_total;
            if (far_mark[v]) ++moved_far;
        }
    }
    CHECK(moved_total > 0);
    // BYTE-IDENTICAL, not merely unchanged to a tolerance.
    CHECK(moved_far == 0);
}

TEST_CASE("the cost of a dab is measured against a cold full reconstruction") {
    MultiresSurface s = build(plane_quads(8, 4.0f), 3);
    const std::uint32_t fine = 3;
    REQUIRE(s.set_display_level(fine));

    // A COLD reconstruction of the whole hierarchy, for the number the local
    // path has to beat.
    s.drop_all_caches();
    s.reset_eval_stats();
    s.positions_at(fine);
    const mesh::MultiresEvalStats cold = s.eval_stats();
    CHECK(cold.full_level_rebuilds == 3);
    CHECK(cold.vertices_evaluated > 0);

    // Then one dab at a coarse level, propagated to the same display level.
    REQUIRE(s.set_sculpt_level(1));
    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0.0f, 0.0f, 0.0f);
    settings.radius = 0.5f;
    settings.strength = 0.4f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);

    s.reset_eval_stats();
    s.positions_at(fine);
    const mesh::MultiresEvalStats local = s.eval_stats();
    CHECK(local.full_level_rebuilds == 0);
    CHECK(local.partial_level_updates == 2);  // levels 2 and 3
    CHECK(local.vertices_evaluated > 0);
    // THE GATE: the dab reconstructs a small fraction of what a cold pass does.
    // Loose on purpose — the ratio is the point, and it improves with depth
    // rather than degrading.
    CHECK(local.vertices_evaluated * 4 < cold.vertices_evaluated);
}

TEST_CASE("growing the hierarchy does not grow the cost of the same dab") {
    // A deeper hierarchy has sixteen times the vertices at the top and the same
    // dab under the brush. What must NOT happen is the cost following the
    // model's size rather than the brush's reach.
    std::uint64_t evaluated[2] = {0, 0};
    std::uint64_t level_size[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const std::uint32_t levels = 2 + static_cast<std::uint32_t>(i);
        MultiresSurface s = build(plane_quads(8, 4.0f), levels);
        REQUIRE(s.set_display_level(2));  // the SAME display level both times
        s.positions_at(2);
        REQUIRE(s.set_sculpt_level(1));
        MultiresSculptor sculptor(s);
        MeshBrushSettings settings;
        settings.center = cf3(0.0f, 0.0f, 0.0f);
        settings.radius = 0.5f;
        settings.strength = 0.4f;
        REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);
        s.reset_eval_stats();
        s.positions_at(2);
        evaluated[i] = s.eval_stats().vertices_evaluated;
        level_size[i] = s.positions_at(s.max_level()).size();
    }
    CHECK(level_size[1] > level_size[0] * 3);
    CHECK(evaluated[0] == evaluated[1]);
}

TEST_CASE("the host drains base patches, not a display mesh") {
    MultiresSurface s = build(plane_quads(6, 3.0f), 2);
    const std::uint32_t patches = s.topology_at(0).face_count;
    s.positions_at(2);
    s.clear_dirty();
    CHECK(s.dirty_patches().empty());

    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0.0f, 0.0f, 0.0f);
    settings.radius = 0.4f;
    settings.strength = 0.4f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);

    const std::vector<std::uint32_t>& dirty = s.dirty_patches();
    CHECK(!dirty.empty());
    // A handful out of thirty-six: what a host uploads is the patches the dab
    // reached, not the display level.
    CHECK(dirty.size() < patches / 2);
    for (std::uint32_t p : dirty) CHECK(p < patches);

    s.clear_dirty();
    CHECK(s.dirty_patches().empty());
}

TEST_CASE("dropping the inactive levels' caches changes nothing") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 3);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0.2f, 0.0f, 0.1f);
    settings.radius = 0.6f;
    settings.strength = 0.5f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);

    REQUIRE(s.set_display_level(3));
    const std::vector<cfloat3> reference = s.positions_at(3);
    const std::uint64_t sum = s.detail_checksum();

    REQUIRE(s.set_display_level(2));
    s.drop_inactive_caches();
    CHECK_FALSE(s.level_resident(3));
    CHECK(s.level_resident(2));
    CHECK(s.detail_checksum() == sum);

    REQUIRE(s.set_display_level(3));
    const std::vector<cfloat3>& rebuilt = s.positions_at(3);
    REQUIRE(rebuilt.size() == reference.size());
    for (std::size_t v = 0; v < rebuilt.size(); ++v) {
        CHECK(rebuilt[v].x == reference[v].x);
        CHECK(rebuilt[v].y == reference[v].y);
        CHECK(rebuilt[v].z == reference[v].z);
    }
    CHECK(s.detail_checksum() == sum);
}

TEST_CASE("the levels between the cage and the brush can be released and stay released") {
    // THE MIDDLE RESIDENCY OPTION. `drop_inactive_caches` releases what is
    // ABOVE the active levels and `drop_all_caches` releases everything; this
    // is the one a host wants while an artist is detailing at a fine level,
    // because a stamp there reads that level's own subdivided positions and
    // frames and nothing else.
    MultiresSurface s = build(plane_quads(6, 3.0f), 3);
    REQUIRE(s.set_sculpt_level(3));
    REQUIRE(s.set_display_level(3));
    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0.0f, 0.0f, 0.0f);
    settings.radius = 0.4f;
    settings.strength = 0.4f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);

    const std::uint64_t sum_before_trim = s.detail_checksum();
    const mesh::MultiresMemory loaded = s.memory();
    CHECK(loaded.resident_levels == 4);

    s.drop_intermediate_caches();
    const mesh::MultiresMemory trimmed = s.memory();
    CHECK(trimmed.resident_levels == 1);  // only the level being worked on
    CHECK(trimmed.rebuildable < loaded.rebuildable);
    CHECK(trimmed.authoritative == loaded.authoritative);
    CHECK(s.detail_checksum() == sum_before_trim);

    // AND THEY STAY RELEASED. Before the evaluation short-circuit this was the
    // half that did not work: the next touch walked from the cage and rebuilt
    // every level it had just released, so the trim bought nothing past the
    // call itself.
    s.reset_eval_stats();
    settings.center = cf3(0.3f, 0.0f, 0.2f);
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);
    s.positions_at(3);
    CHECK(s.eval_stats().full_level_rebuilds == 0);
    CHECK(s.memory().resident_levels == 1);

    // The surface the trimmed hierarchy holds is what a full one would hold,
    // which is the property the rebuild below checks against.
    const std::vector<cfloat3> reference = s.positions_at(3);
    const std::uint64_t sum = s.detail_checksum();

    // What it costs is an edit BELOW the active levels, which rebuilds what it
    // needs — and reconstructs the same surface doing it.
    REQUIRE(s.set_sculpt_level(0));
    const std::vector<cfloat3> after_rebuild = s.positions_at(3);
    REQUIRE(after_rebuild.size() == reference.size());
    for (std::size_t v = 0; v < after_rebuild.size(); ++v) {
        CHECK(after_rebuild[v].x == reference[v].x);
        CHECK(after_rebuild[v].y == reference[v].y);
        CHECK(after_rebuild[v].z == reference[v].z);
    }
    CHECK(s.detail_checksum() == sum);
}

TEST_CASE("a stamp on a level nobody is displaying still propagates when asked") {
    // Lazy evaluation: sculpting at level 1 while displaying level 1 must not
    // rebuild level 3 on the spot. It must rebuild it when something asks.
    MultiresSurface s = build(plane_quads(4, 2.0f), 3);
    s.positions_at(3);
    REQUIRE(s.set_sculpt_level(1));
    REQUIRE(s.set_display_level(1));

    MultiresSculptor sculptor(s);
    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 1.0f;
    settings.strength = 0.5f;
    const std::vector<cfloat3> fine_before = s.positions_at(3);

    s.reset_eval_stats();
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings) > 0);
    s.positions_at(1);
    // Nothing above the level being worked on was touched.
    CHECK(s.eval_stats().partial_level_updates == 0);
    CHECK(s.eval_stats().full_level_rebuilds == 0);

    const std::vector<cfloat3>& fine_after = s.positions_at(3);
    CHECK(s.eval_stats().partial_level_updates == 2);
    bool moved = false;
    for (std::size_t v = 0; v < fine_after.size(); ++v)
        moved = moved || fine_after[v].y != fine_before[v].y;
    CHECK(moved);
}
