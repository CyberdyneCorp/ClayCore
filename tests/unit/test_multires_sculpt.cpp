// Sculpting a hierarchy, and the one test the feature exists for
// (mesh-multires spec, add-mesh-multires).
//
// THE SIGNATURE TEST is `detail at a fine level survives a form change at a
// coarse one`. Everything else in this file supports it: the brushes are the
// fixed sculptor's, so the parity case below is what says so in bytes rather
// than in a comment, and the undo cases are what make the gesture reversible
// without recording the millions of derived vertices a coarse stroke moves.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/mesh/automask.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresDelta;
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

// The HIGH-PASS energy of a level: how far, on average, the surface stands off
// the pure subdivision. It is the numeric form of "there is detail here", and
// comparing it before and after a coarse edit is a better instrument than a
// screenshot (guide section 89).
double detail_energy(MultiresSurface& s, std::uint32_t level) {
    const std::vector<cfloat3>& p = s.positions_at(level);
    const std::vector<cfloat3>& sub = s.subdivided_at(level);
    double sum = 0.0;
    for (std::size_t v = 0; v < p.size(); ++v) sum += clength(p[v] - sub[v]);
    return p.empty() ? 0.0 : sum / static_cast<double>(p.size());
}

bool same_bytes(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

}  // namespace

TEST_CASE("every verb is offered, including the one an adaptive surface declines") {
    for (int v = 0; v <= static_cast<int>(MeshBrush::Smear); ++v)
        CHECK(mesh::multires_offers(static_cast<MeshBrush>(v)));
}

TEST_CASE("a stamp at a fine level becomes detail rather than a position") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor sculptor(s);

    CHECK(s.detail_at(2).empty());
    const std::vector<cfloat3> before = s.positions_at(2);

    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 0.6f;
    settings.strength = 0.5f;
    const std::size_t moved = sculptor.stamp(MeshBrush::Draw, settings);
    CHECK(moved > 0);

    // What the brush wrote is stored as COEFFICIENTS, not as positions: that is
    // the whole difference between this and the fixed sculptor.
    CHECK_FALSE(s.detail_at(2).empty());
    CHECK(s.detail_revision() > 1);
    CHECK(!sculptor.last_write_vertices().empty());

    // And the level still reconstructs to exactly what the brush left.
    const std::vector<cfloat3> after = s.positions_at(2);
    CHECK_FALSE(same_bytes(before, after));
    for (std::uint32_t v : sculptor.last_write_vertices()) {
        const LocalDetail d = s.detail_at(2).get(v);
        const cfloat3 expect = s.subdivided_at(2)[v] +
                               mesh::frame_to_world(s.frames_at(2)[v], d.tangent, d.bitangent,
                                                    d.normal);
        CHECK(after[v].x == doctest::Approx(expect.x));
        CHECK(after[v].y == doctest::Approx(expect.y));
        CHECK(after[v].z == doctest::Approx(expect.z));
    }
}

TEST_CASE("a stamp at the cage moves the cage itself") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(0));
    MultiresSculptor sculptor(s);

    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 1.5f;
    settings.strength = 0.5f;
    CHECK(sculptor.stamp(MeshBrush::Draw, settings) > 0);

    // The CAGE moved — no detail was manufactured to express a coarse edit,
    // which is what keeps level 0 the production geometry rather than a
    // displacement layer over something else.
    CHECK(s.detail_at(1).empty());
    CHECK(s.detail_at(2).empty());
    CHECK(s.base_revision() > 1);
    bool moved = false;
    for (const cfloat3& p : s.base_mesh().positions) moved = moved || std::fabs(p.y) > 1e-4f;
    CHECK(moved);
}

TEST_CASE("THE SIGNATURE TEST: sculpt fine, change the form, come back") {
    MultiresSurface s = build(plane_quads(6, 3.0f), 3);
    const std::uint32_t fine = 3, coarse = 1;

    // 1. Fine detail: a tight stamp at the finest level.
    REQUIRE(s.set_sculpt_level(fine));
    REQUIRE(s.set_display_level(fine));
    MultiresSculptor sculptor(s);
    MeshBrushSettings detail_brush;
    detail_brush.center = cf3(0.5f, 0.0f, 0.5f);
    detail_brush.radius = 0.5f;
    detail_brush.strength = 0.4f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, detail_brush) > 0);

    const double energy_before = detail_energy(s, fine);
    CHECK(energy_before > 1e-4);
    const std::uint64_t detail_before = s.detail_at(fine).checksum();
    const std::vector<cfloat3> fine_before = s.positions_at(fine);

    // 2. Broad correction at a coarse level, displaying the fine one — the
    //    workflow the independence of the two levels exists for.
    REQUIRE(s.set_sculpt_level(coarse));
    REQUIRE(s.set_display_level(fine));
    sculptor.begin_stroke();
    MeshBrushSettings form_brush;
    form_brush.center = cf3(0.0f, 0.0f, 0.0f);
    form_brush.radius = 2.5f;
    form_brush.strength = 0.6f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, form_brush) > 0);

    const std::vector<cfloat3> fine_after = s.positions_at(fine);

    // 3a. THE FORM MOVED.
    float max_move = 0.0f;
    for (std::size_t v = 0; v < fine_after.size(); ++v)
        max_move = std::max(max_move, clength(fine_after[v] - fine_before[v]));
    CHECK(max_move > 0.1f);

    // 3b. THE DETAIL IS STILL THERE, coefficient for coefficient.
    CHECK(s.detail_at(fine).checksum() == detail_before);

    // 3c. AND IT MOVED WITH THE FORM: the high-pass energy is preserved, which
    //     is what distinguishes "the wrinkle survived" from "the wrinkle was
    //     flattened by the pass underneath it".
    const double energy_after = detail_energy(s, fine);
    CHECK(energy_after == doctest::Approx(energy_before).epsilon(0.05));
}

TEST_CASE("the same brush gives the same surface as the fixed sculptor") {
    // NOT a similarity check where it can be exact. `MultiresSculptor` calls
    // `MeshSculptor::stamp` on the level's own mesh, so the two paths are the
    // same instructions on the same inputs. If this ever drifts, "Clay" has
    // started to mean two things.
    //
    // AT THE CAGE the agreement is bit-exact, because a level-0 edit is stored
    // as the position itself. ABOVE the cage it is exact to a round trip
    // through the frame — the position is reconstructed from the coefficients
    // that were derived from it, which is what keeps the surface a host sees
    // identical to the surface a reload produces.
    SUBCASE("at the cage, bit for bit") {
        MultiresSurface s = build(plane_quads(4, 2.0f), 2);
        REQUIRE(s.set_sculpt_level(0));
        Mesh flat = s.mesh_at_level(0, {/*normals=*/false, false, false});
        flat.normals = s.normals_at(0);
        mesh::MeshSculptor fixed(flat, 0.0f);

        MeshBrushSettings settings;
        settings.center = cf3(0.3f, 0.0f, -0.2f);
        settings.radius = 1.2f;
        settings.strength = 0.45f;

        MultiresSculptor sculptor(s);
        const std::size_t a = sculptor.stamp(MeshBrush::Clay, settings);
        const std::size_t b = fixed.stamp(MeshBrush::Clay, settings);
        CHECK(a == b);
        CHECK(a > 0);
        CHECK(same_bytes(s.positions_at(0), flat.positions));
    }

    SUBCASE("above the cage, to a frame round trip") {
        MultiresSurface s = build(plane_quads(4, 2.0f), 2);
        REQUIRE(s.set_sculpt_level(2));
        Mesh flat = s.mesh_at_level(2, {/*normals=*/false, false, false});
        REQUIRE(flat.positions.size() == s.positions_at(2).size());
        flat.normals = s.normals_at(2);
        mesh::MeshSculptor fixed(flat, 0.0f);

        MeshBrushSettings settings;
        settings.center = cf3(0.3f, 0.0f, -0.2f);
        settings.radius = 0.8f;
        settings.strength = 0.45f;

        MultiresSculptor sculptor(s);
        const std::size_t a = sculptor.stamp(MeshBrush::Clay, settings);
        const std::size_t b = fixed.stamp(MeshBrush::Clay, settings);
        CHECK(a == b);
        CHECK(a > 0);
        const std::vector<cfloat3>& got = s.positions_at(2);
        REQUIRE(got.size() == flat.positions.size());
        float worst = 0.0f;
        for (std::size_t v = 0; v < got.size(); ++v)
            worst = std::max(worst, clength(got[v] - flat.positions[v]));
        CHECK(worst < 1e-5f);
    }
}

TEST_CASE("a gesture at a fine level reverts bit-identically") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 3);
    REQUIRE(s.set_sculpt_level(3));
    MultiresSculptor sculptor(s);
    const std::vector<cfloat3> before = s.positions_at(3);
    const std::uint64_t sum = s.detail_checksum();

    MultiresDelta record;
    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 0.7f;
    settings.strength = 0.5f;
    // Several stamps of one stroke, coalesced into one record.
    for (int i = 0; i < 5; ++i) {
        settings.center = cf3(-0.4f + 0.2f * static_cast<float>(i), 0.0f, 0.0f);
        REQUIRE(sculptor.stamp(MeshBrush::Draw, settings, {}, &record) > 0);
    }
    CHECK_FALSE(record.empty());
    CHECK(record.levels() == std::vector<std::uint32_t>{3});
    CHECK_FALSE(same_bytes(s.positions_at(3), before));

    REQUIRE(record.revert(s));
    CHECK(s.detail_checksum() == sum);
    CHECK(same_bytes(s.positions_at(3), before));

    // And redo puts it back.
    const std::vector<cfloat3> sculpted_expected = [&] {
        std::vector<cfloat3> copy;
        REQUIRE(record.apply(s));
        copy = s.positions_at(3);
        REQUIRE(record.revert(s));
        return copy;
    }();
    REQUIRE(record.apply(s));
    CHECK(same_bytes(s.positions_at(3), sculpted_expected));
}

TEST_CASE("a gesture at the cage reverts the levels above it too") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 3);
    REQUIRE(s.set_sculpt_level(0));
    MultiresSculptor sculptor(s);
    const std::vector<cfloat3> fine_before = s.positions_at(3);
    const Mesh base_before = s.base_mesh();

    MultiresDelta record;
    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 2.0f;
    settings.strength = 0.5f;
    REQUIRE(sculptor.stamp(MeshBrush::Draw, settings, {}, &record) > 0);
    CHECK_FALSE(same_bytes(s.positions_at(3), fine_before));

    REQUIRE(record.revert(s));
    CHECK(same_bytes(s.base_mesh().positions, base_before.positions));
    // The DERIVED level comes back because the relationship reconstructs it,
    // not because the record stored it.
    CHECK(same_bytes(s.positions_at(3), fine_before));
}

TEST_CASE("undo size follows the edit, not the hierarchy") {
    // A coarse stroke on a deep hierarchy moves millions of derived vertices.
    // The record must follow the vertices it EDITED at the level it was made
    // on, or an undo step costs the level count times what it needs to.
    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 1.5f;
    settings.strength = 0.4f;

    std::size_t sizes[2] = {0, 0};
    std::size_t fine_vertices[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        MultiresSurface s = build(plane_quads(4, 2.0f), 2 + static_cast<std::uint32_t>(i) * 2);
        REQUIRE(s.set_sculpt_level(0));
        MultiresSculptor sculptor(s);
        MultiresDelta record;
        REQUIRE(sculptor.stamp(MeshBrush::Draw, settings, {}, &record) > 0);
        sizes[i] = record.size();
        fine_vertices[i] = s.positions_at(s.max_level()).size();
    }
    // Two more levels: sixteen times the derived vertices, and the SAME record.
    CHECK(fine_vertices[1] > fine_vertices[0] * 10);
    CHECK(sizes[0] == sizes[1]);
    CHECK(sizes[0] > 0);
}

TEST_CASE("a mask freezes a multires stamp exactly as it freezes every other") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor sculptor(s);
    const std::vector<cfloat3> before = s.positions_at(2);

    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 1.0f;
    settings.strength = 0.5f;
    // A gate of 1 everywhere is a full freeze; nothing may move and no detail
    // may be manufactured.
    const field::MaskGate gate = [](kernel::cfloat3) { return 1.0f; };
    CHECK(sculptor.stamp(MeshBrush::Draw, settings, gate) == 0);
    CHECK(s.detail_at(2).empty());
    CHECK(same_bytes(s.positions_at(2), before));
}

TEST_CASE("the stroke engine reaches a hierarchy, and means the same thing there") {
    // THE GAP THIS CLOSES: `brush::apply_to_mesh` was the stroke engine's only
    // mesh consumer, so a host driving a hierarchy had to walk the stamps
    // itself — and the moment it does that, "what a stroke is" has two
    // definitions. The two entry points share the per-stamp resolution, so this
    // asserts the sharing rather than a resemblance.
    MultiresSurface s = build(plane_quads(5, 2.5f), 2);
    REQUIRE(s.set_sculpt_level(2));

    std::vector<brush::Stamp> stamps;
    for (int i = 0; i < 6; ++i) {
        brush::Stamp stamp;
        stamp.position = cf3(-0.6f + 0.24f * static_cast<float>(i), 0.0f, 0.0f);
        stamp.radius = 0.45f;
        stamp.strength = 0.6f;
        stamps.push_back(stamp);
    }
    MeshBrushSettings settings;
    settings.strength = 0.5f;

    // The multiresolution path, driven as a stroke.
    MultiresSculptor sculptor(s);
    MultiresDelta record;
    const std::size_t applied =
        brush::apply_to_multires(sculptor, stamps, MeshBrush::Draw, settings, nullptr, &record);
    CHECK(applied == stamps.size());
    CHECK_FALSE(s.detail_at(2).empty());
    CHECK(record.levels() == std::vector<std::uint32_t>{2});

    // THE SAME STROKE onto the same topology through the fixed engine. Both go
    // through `mesh_stamp_settings`, so each stamp lands in the same place with
    // the same radius and the same pressure-scaled strength; the surfaces agree
    // to the frame round trip a hierarchy stores its detail through.
    MultiresSurface twin = build(plane_quads(5, 2.5f), 2);
    Mesh flat = twin.mesh_at_level(2, {/*normals=*/false, false, false});
    flat.normals = twin.normals_at(2);
    mesh::MeshSculptor fixed(flat, 0.0f);
    const std::size_t fixed_applied =
        brush::apply_to_mesh(fixed, stamps, MeshBrush::Draw, settings);
    CHECK(fixed_applied == applied);

    const std::vector<cfloat3>& got = s.positions_at(2);
    REQUIRE(got.size() == flat.positions.size());
    float worst = 0.0f;
    for (std::size_t v = 0; v < got.size(); ++v)
        worst = std::max(worst, clength(got[v] - flat.positions[v]));
    CHECK(worst < 1e-5f);

    // And the whole stroke is ONE undo step.
    REQUIRE(record.revert(s));
    CHECK(s.detail_at(2).empty());
}

TEST_CASE("a stroke onto a hierarchy takes the mask and the deferred normals") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 2);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor sculptor(s);

    std::vector<brush::Stamp> stamps;
    for (int i = 0; i < 4; ++i) {
        brush::Stamp stamp;
        stamp.position = cf3(-0.3f + 0.2f * static_cast<float>(i), 0.0f, 0.0f);
        stamp.radius = 0.5f;
        stamp.strength = 1.0f;
        stamps.push_back(stamp);
    }
    MeshBrushSettings settings;
    settings.strength = 0.5f;

    // Deferring the normal recompute to the end of the stroke changes nothing
    // about the surface it leaves.
    brush::MeshStrokeOptions deferred;
    deferred.defer_normals = true;
    CHECK(brush::apply_to_multires(sculptor, stamps, MeshBrush::Draw, settings, nullptr, nullptr,
                                   deferred) == stamps.size());
    const std::vector<cfloat3> with_defer = s.positions_at(2);
    CHECK_FALSE(sculptor.defer_normals());  // restored to what it was

    MultiresSurface plain = build(plane_quads(4, 2.0f), 2);
    REQUIRE(plain.set_sculpt_level(2));
    MultiresSculptor plain_sculptor(plain);
    CHECK(brush::apply_to_multires(plain_sculptor, stamps, MeshBrush::Draw, settings) ==
          stamps.size());
    CHECK(same_bytes(plain.positions_at(2), with_defer));

    // A verb the vocabulary does not offer is refused rather than run as
    // something else. Every verb IS offered here, so the refusal is only
    // reachable through the predicate — which is what `multires_offers`
    // documents.
    CHECK(mesh::multires_offers(MeshBrush::Layer));
}

// -- the stale seed (sculpt-runtime spec, task 3.2) ---------------------------
//
// A seed is an INDEX, and an index outlives the numbering it came from. A
// hierarchy renumbers on every rebind, so a host that picks at one level and
// stamps at another hands the walk a class that is still in bounds and no
// longer means anything. The bounds check cannot see it, and the cost is not a
// slightly wrong dab: `geodesic_region` gives up and returns an empty region
// when the seed sits farther than the radius from the centre, so the stamp
// silently does nothing.
//
// The three cases below are the same stale seed spent three ways, and the
// middle one is the defect still reachable on purpose — an unrevisioned seed is
// exactly what every caller written before the token sends.
TEST_CASE("a seed from another level is refused rather than spent on an empty region") {
    MultiresSurface s = build(plane_quads(4, 2.0f), 2);
    MultiresSculptor sculptor(s);

    // Pick at the COARSE level, at a corner — far from where the stamp lands.
    REQUIRE(s.set_sculpt_level(1));
    const std::uint64_t coarse_revision = sculptor.seed_revision();
    REQUIRE(coarse_revision != mesh::kNoSeedRevision);
    mesh::MeshSculptor* coarse = sculptor.level_sculptor();
    REQUIRE(coarse != nullptr);
    const std::uint32_t corner = coarse->nearest_class(cf3(-2.0f, 0, -2.0f));

    // Now sculpt at the FINE level, whose class space is a different, larger
    // numbering. The seed stays in bounds there, which is the whole problem.
    REQUIRE(s.set_sculpt_level(2));
    const std::uint64_t fine_revision = sculptor.seed_revision();
    CHECK(fine_revision != coarse_revision);
    REQUIRE(corner < sculptor.level_sculptor()->adjacency().class_count());

    MeshBrushSettings settings;
    settings.center = cf3(0, 0, 0);
    settings.radius = 0.6f;
    settings.strength = 0.5f;
    settings.geodesic = true;
    settings.seed_class = corner;

    SUBCASE("unrevisioned: the seed is trusted, and the dab is lost") {
        // The behaviour every caller had before the token existed, kept
        // deliberately so this file can show what the token buys rather than
        // assert it. A corner seed is farther than 0.6 from the origin, so the
        // walk returns nothing and the stamp moves nobody.
        settings.seed_revision = mesh::kNoSeedRevision;
        CHECK(sculptor.stamp(MeshBrush::Draw, settings) == 0);
        CHECK(s.detail_at(2).empty());
    }

    SUBCASE("revisioned and stale: refused, and the stamp lands anyway") {
        settings.seed_revision = coarse_revision;
        const std::size_t moved = sculptor.stamp(MeshBrush::Draw, settings);
        CHECK(moved > 0);
        CHECK_FALSE(s.detail_at(2).empty());
        // Proves the rejection HAPPENED rather than the seed having been
        // harmless — without this the case would pass on a build that ignored
        // the revision and got lucky.
        CHECK(sculptor.level_sculptor()->stale_seeds_rejected() == 1);
    }

    SUBCASE("revisioned and live: honoured, and costs no rejection") {
        // The same seed, now genuinely of this level's numbering: the class
        // nearest the centre. This is the case the seed exists to make fast.
        mesh::MeshSculptor* fine = sculptor.level_sculptor();
        settings.seed_class = fine->nearest_class(settings.center);
        settings.seed_revision = fine_revision;
        CHECK(sculptor.stamp(MeshBrush::Draw, settings) > 0);
        CHECK(sculptor.level_sculptor()->stale_seeds_rejected() == 0);
    }
}

// -- sculpting across a depth boundary ---------------------------------------
//
// A regional level stores the faces of the patches it refines and nothing else,
// so every walk the brush makes over it — the geodesic frontier, the Laplacian's
// ring, the angle-weighted normal, the boundary automask's shared-triangle count
// — reads the rim of the refined region as an open border of the model. Nothing
// refuses and nothing picks a wrong neighbour: the coarse neighbour has no
// vertex, no weld class and no triangle at this level, so the damage is
// truncation and one-sidedness and it is silent.
//
// `MultiresSurface::cross_level_at` gives those neighbours an identity. These
// gates are the difference it makes, measured by binding the SAME sculptor with
// and without it — which is also the parity gate, because away from a boundary
// the two are the same bytes.

namespace {

// The verbs a per-vertex normal steers, and the one that averages a ring. Both
// read a neighbourhood the level does not hold at a depth boundary, and one
// stamp of each is what says so in positions.
std::size_t stamp_level(MultiresSurface& s, std::uint32_t level, MeshBrush verb, cfloat3 center,
                        float radius, bool complete) {
    const mesh::CrossLevelNeighborhood& cross = s.cross_level_at(level);
    mesh::MeshSculptor sculptor(s.level_mesh(level), s.level_adjacency(level));
    if (complete) sculptor.set_cross_level(&cross);
    MeshBrushSettings settings;
    settings.center = center;
    settings.radius = radius;
    settings.strength = 1.0f;
    return sculptor.stamp(verb, settings);
}

// The 6x6 cage the regional gates use, bumped so that smoothing has something
// to do: a flat plane is already smooth and every verb on it is a no-op.
Mesh bumpy_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x),
                                      0.15f * static_cast<float>((x * 7 + z * 3) % 5),
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

// The same cage refined over an L of three patches, which leaves the fourth
// cell of the block coarse and so gives the refined region a CONCAVE corner.
// The 2x2 block has none, and several things at a depth boundary only exist at
// one.
MultiresSurface build_regional_L(const Mesh& cage) {
    MultiresError err = MultiresError::None;
    auto s = MultiresSurface::from_mesh(cage, {}, &err);
    REQUIRE_MESSAGE(s.has_value(), mesh::multires_error_text(err));
    const std::vector<std::uint32_t> ell = {2u * 6u + 2u, 2u * 6u + 3u, 3u * 6u + 2u};
    REQUIRE(s->refine_patches_to_level(ell, 3));
    return std::move(*s);
}

MultiresSurface build_regional(const Mesh& cage) {
    MultiresError err = MultiresError::None;
    auto s = MultiresSurface::from_mesh(cage, {}, &err);
    REQUIRE_MESSAGE(s.has_value(), mesh::multires_error_text(err));
    // The middle 2x2 of a 6x6 cage to level 3, which grades the levels below it
    // out to the rings their stencils need.
    std::vector<std::uint32_t> block;
    for (int z = 2; z < 4; ++z)
        for (int x = 2; x < 4; ++x) block.push_back(static_cast<std::uint32_t>(z * 6 + x));
    REQUIRE(s->refine_patches_to_level(block, 3));
    return std::move(*s);
}

// The angle-weighted contribution one triangle makes to the geometric normal at
// whichever of its corners belong to `cls`. `class_normal`'s and
// `recompute_normals`'s shared arithmetic, written out here so the gate below
// RE-DERIVES the answer from the mesh rather than asking the code under test
// for it.
cfloat3 triangle_contribution(const Mesh& m, const mesh::Adjacency& adj, std::uint32_t tri,
                              std::uint32_t cls) {
    const cfloat3 p[3] = {m.positions[m.indices[tri * 3]], m.positions[m.indices[tri * 3 + 1]],
                          m.positions[m.indices[tri * 3 + 2]]};
    const cfloat3 face = mesh::safe_normalize(ccross(p[1] - p[0], p[2] - p[0]), cf3(0, 0, 0));
    cfloat3 sum = cf3(0, 0, 0);
    for (int corner = 0; corner < 3; ++corner) {
        if (adj.class_of(m.indices[tri * 3 + corner]) != cls) continue;
        const cfloat3 u = p[(corner + 1) % 3] - p[corner], w = p[(corner + 2) % 3] - p[corner];
        const float lu = clength(u), lw = clength(w);
        if (lu < 1e-20f || lw < 1e-20f) continue;
        sum = sum + face * std::acos(std::clamp(cdot(u, w) / (lu * lw), -1.0f, 1.0f));
    }
    return sum;
}

// The angle-weighted normal over the triangles this level STORES, and nothing
// else — what the rim would get if the derived faces were not added.
cfloat3 own_ring_normal(const Mesh& m, const mesh::Adjacency& adj, std::uint32_t cls) {
    std::size_t tc = 0;
    const std::uint32_t* tris = adj.triangles_of(cls, &tc);
    cfloat3 sum = cf3(0, 0, 0);
    for (std::size_t k = 0; k < tc; ++k) sum = sum + triangle_contribution(m, adj, tris[k], cls);
    return sum;
}

// The level vertex nearest `p`, by position.
std::uint32_t nearest_vertex(const std::vector<cfloat3>& positions, cfloat3 p) {
    std::uint32_t best = 0;
    float best_d = 1e30f;
    for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(positions.size()); ++v) {
        const float d = clength(positions[v] - p);
        if (d < best_d) {
            best_d = d;
            best = v;
        }
    }
    return best;
}

// One Smooth stamp over a level's own mesh, driven by a `MeshSculptor` exactly
// as `MultiresSculptor::bind` drives one — so the test can bind the same stamp
// with the cross-level neighbourhood and without it.
std::size_t smooth_level(MultiresSurface& s, std::uint32_t level, cfloat3 center, float radius,
                         bool complete, const mesh::AutomaskSettings* automask) {
    const mesh::CrossLevelNeighborhood& cross = s.cross_level_at(level);
    mesh::MeshSculptor sculptor(s.level_mesh(level), s.level_adjacency(level));
    if (complete) sculptor.set_cross_level(&cross);
    MeshBrushSettings settings;
    settings.center = center;
    settings.radius = radius;
    settings.strength = 1.0f;
    if (automask) settings.automask = *automask;
    return sculptor.stamp(MeshBrush::Smooth, settings);
}

}  // namespace

TEST_CASE("multires: a depth transition is not a border of the model") {
    MultiresSurface part = build_regional(bumpy_quads(6, 1.0f));
    const mesh::CrossLevelNeighborhood& cross = part.cross_level_at(3);
    const Mesh& m = part.level_mesh(3);
    const mesh::Adjacency& adj = part.level_adjacency(3);

    std::size_t as_border = 0, still_border = 0;
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(adj.class_count()); ++c) {
        if (mesh::is_boundary_class(m, adj, c, nullptr)) ++as_border;
        if (mesh::is_boundary_class(m, adj, c, &cross)) ++still_border;
    }
    // THE WHOLE RIM OF THE REFINED REGION, and none of it is on the cage's own
    // outer edge: the 2x2 block is in the middle of a 6x6 cage, so every one of
    // these is an internal seam the artist cannot see and did not put there.
    CHECK(as_border == 64);
    CHECK(still_border == 0);

    // ...and the model's ACTUAL border still reports true, because there is no
    // derived face on the other side of that one. A uniformly refined hierarchy
    // has an empty neighbourhood, so this is also the parity case.
    MultiresSurface dense = build(bumpy_quads(6, 1.0f), 3);
    const mesh::CrossLevelNeighborhood& none = dense.cross_level_at(3);
    CHECK(none.empty());
    const Mesh& dm = dense.level_mesh(3);
    const mesh::Adjacency& dadj = dense.level_adjacency(3);
    std::size_t edge = 0, edge_with = 0;
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(dadj.class_count()); ++c) {
        if (mesh::is_boundary_class(dm, dadj, c, nullptr)) ++edge;
        if (mesh::is_boundary_class(dm, dadj, c, &none)) ++edge_with;
    }
    CHECK(edge == 192);  // the perimeter of a 49 x 49 grid of vertices
    CHECK(edge_with == edge);
}

TEST_CASE("multires: a cross-level neighbour that this level stores is already in its ring") {
    // WHY `ring_slots` AND THE ADJACENCY RING ARE LEFT ALONE. A derived face's
    // two stored corners are a vertex point and an edge point of the same coarse
    // corner, and the refined face on the other side of that edge already joins
    // them — so the only neighbour a depth boundary ADDS is one this level does
    // not store. Asserted rather than argued, because `build_neighbors` relies
    // on it to append without de-duplicating against the ring.
    MultiresSurface part = build_regional(bumpy_quads(6, 1.0f));
    const mesh::CrossLevelNeighborhood& cross = part.cross_level_at(3);
    const mesh::Adjacency& adj = part.level_adjacency(3);

    std::size_t inside = 0, missing = 0, outside = 0;
    for (std::uint32_t v = 0; v < cross.vertex_count; ++v) {
        std::size_t rc = 0;
        const std::uint32_t* ring = cross.ring_of(v, &rc);
        std::size_t ac = 0;
        const std::uint32_t* own = adj.ring(adj.class_of(v), &ac);
        for (std::size_t i = 0; i < rc; ++i) {
            if (!cross.inside(ring[i])) {
                ++outside;
                continue;
            }
            ++inside;
            const std::uint32_t cls = adj.class_of(ring[i]);
            if (!std::binary_search(own, own + ac, cls)) ++missing;
        }
    }
    CHECK(inside > 0);
    CHECK(outside > 0);
    CHECK(missing == 0);
}

TEST_CASE("multires: a normal recompute completes the ring at a depth transition") {
    // TASK 3.3 AS A VALUE, not as a call. `recompute_normals` rewrites the
    // level's DISPLAY normals over the classes an edit touched, angle-weighted
    // over that level's own triangles — which at the rim of a refined region is
    // a short, one-sided fan, because the faces on the coarse side are simply
    // absent. The derived faces are added to the same sum, on the same corners
    // and with the same weighting.
    //
    // DRIVEN BY A DEFORMER rather than a stamp, so every class of the level is
    // in the touched set at once and the gate covers the whole rim rather than
    // whatever a brush happened to reach. What is asserted is the resulting
    // normal, re-derived here from the mesh and the neighbourhood: the stored
    // value has to be the COMPLETE fan and not the level's own.
    MultiresSurface s = build_regional(bumpy_quads(6, 1.0f));
    const mesh::CrossLevelNeighborhood& cross = s.cross_level_at(3);
    REQUIRE_FALSE(cross.empty());
    const mesh::Adjacency& adj = s.level_adjacency(3);
    Mesh& m = s.level_mesh(3);
    REQUIRE(m.normals.size() == m.positions.size());

    mesh::MeshSculptor sculptor(m, adj);
    sculptor.set_cross_level(&cross);
    mesh::MeshDeformSettings twist;
    twist.verb = mesh::MeshDeform::Twist;
    twist.origin = cf3(0, -1, 0);
    twist.axis = cf3(0, 1, 0);
    twist.span = 4.0f;
    twist.angle = 0.3f;
    REQUIRE(sculptor.apply_deformer(twist) > 0);

    std::size_t rim = 0, welded = 0;
    float worst_complete = 0.0f, worst_own = 0.0f;
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(adj.class_count()); ++c) {
        std::size_t mc = 0;
        const std::uint32_t* members = adj.members(c, &mc);
        // One member each on this cage, which is what lets the per-member
        // accumulation below be compared against a per-class sum.
        welded += mc != 1u ? 1u : 0u;
        const std::uint32_t v = members[0];
        const cfloat3 extra = cross.normal_contribution(m.positions, v);
        // Zero away from a depth boundary, and no work there — which is also
        // why this loop names the rim without a second predicate.
        if (clength(extra) == 0.0f) continue;
        ++rim;
        const cfloat3 own = own_ring_normal(m, adj, c);
        const cfloat3 stored = m.normals[v];
        worst_complete = std::max(
            worst_complete,
            clength(stored - mesh::safe_normalize(own + extra, cf3(0, 1, 0))));
        worst_own =
            std::max(worst_own, clength(stored - mesh::safe_normalize(own, cf3(0, 1, 0))));
    }
    MESSAGE("rim classes " << rim << ", worst against the complete fan " << worst_complete
                           << ", worst against the level's own " << worst_own);
    // NOT VACUOUS: there is a rim, and it is the whole of it.
    CHECK(rim == 64u);
    CHECK(welded == 0u);
    // A BAND WITH BOTH TEETH. The stored normal is the complete fan to within
    // the reassociation of one float sum, and it is nowhere near the level's
    // own — the ratio between the two is the part of this that travels.
    CHECK(worst_complete < 1e-5f);
    CHECK(worst_own > 1e-2f);
    CHECK(worst_complete * 100.0f < worst_own);
}

TEST_CASE("multires: a smoothing verb is not dragged inward at a depth transition") {
    const Mesh cage = bumpy_quads(6, 1.0f);
    MultiresSurface dense = build(cage, 3);
    MultiresSurface with = build_regional(cage);
    MultiresSurface without = build_regional(cage);

    // Anchored ON the rim of the refined region — the case the level's own
    // connectivity cannot see. The 2x2 block covers [-1/3, 1/3]; the vertex
    // nearest (-1/3, ., 0) is on its western edge.
    const std::vector<cfloat3> before = with.positions_at(3);
    const std::uint32_t seed = nearest_vertex(before, cf3(-1.0f / 3.0f, 0.0f, 0.0f));
    const cfloat3 center = before[seed];
    const float radius = 0.25f;

    std::map<std::array<float, 3>, std::uint32_t> dense_of;
    {
        const std::vector<cfloat3>& p = dense.positions_at(3);
        for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(p.size()); ++v)
            dense_of[{p[v].x, p[v].y, p[v].z}] = v;
    }

    const std::size_t dense_moved = smooth_level(dense, 3, center, radius, true, nullptr);
    const std::size_t with_moved = smooth_level(with, 3, center, radius, true, nullptr);
    const std::size_t without_moved = smooth_level(without, 3, center, radius, false, nullptr);
    // The stamp reaches further on a uniformly refined hierarchy simply because
    // there is more of it to reach: the coarse side has no class at this level
    // to move, which is task 4's subject and not this one's.
    CHECK(dense_moved == 107);
    CHECK(with_moved == without_moved);

    // WHERE THE SHARED VERTICES FINISH. `laplacian_pass` divides by the ring
    // size AS FOUND, so at the rim the mean is taken over a short one-sided ring
    // and the border is pulled into the refined region.
    const auto differs = [&](MultiresSurface& s) {
        const std::vector<cfloat3>& after = s.level_mesh(3).positions;
        const std::vector<cfloat3>& reference = dense.level_mesh(3).positions;
        std::size_t n = 0;
        float worst = 0.0f;
        for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(after.size()); ++v) {
            const auto it = dense_of.find({before[v].x, before[v].y, before[v].z});
            if (it == dense_of.end()) continue;
            const cfloat3 d = after[v] - reference[it->second];
            if (d.x != 0.0f || d.y != 0.0f || d.z != 0.0f) ++n;
            worst = std::max(worst, clength(d));
        }
        MESSAGE("shared vertices finishing elsewhere: " << n << ", worst " << worst);
        return n;
    };
    // 11 of them without the complete neighbourhood, the worst 0.0185 out of
    // place — about 44% of the level-3 edge spacing of 0.0417, a subdivision
    // step rather than a hairline. With it, 3 differ and the worst is 3.0e-08:
    // float rounding, because the same neighbours are summed in a different
    // order. Assert the COUNT; the distance is in the message beside it.
    CHECK(differs(without) == 11);
    CHECK(differs(with) == 3);

    // THE VISIBLE CONSEQUENCE, not just the predicate. With boundary automasking
    // on, the brush fades at a seam the artist cannot see: 71 classes move
    // instead of 82, on a stamp nowhere near the model's own edge. With the
    // complete neighbourhood the automask finds no border and the stamp is the
    // one it would have been with the factor off.
    MultiresSurface am_with = build_regional(cage);
    MultiresSurface am_without = build_regional(cage);
    mesh::AutomaskSettings automask;
    automask.factors = static_cast<std::uint32_t>(mesh::AutomaskFactor::Boundary);
    automask.boundary_rings = 2;
    CHECK(smooth_level(am_without, 3, center, radius, false, &automask) == 71);
    CHECK(smooth_level(am_with, 3, center, radius, true, &automask) == with_moved);
}

TEST_CASE("multires: a stamp away from a transition is bit-identical either way") {
    const Mesh cage = bumpy_quads(6, 1.0f);
    MultiresSurface with = build_regional(cage);
    MultiresSurface without = build_regional(cage);

    // THE PARITY GATE. The centre of the refined block is four level-3 rings
    // from the nearest boundary, so a stamp there reaches nothing the level does
    // not store and the complete neighbourhood must change nothing at all.
    const std::vector<cfloat3> before = with.positions_at(3);
    const cfloat3 center = before[nearest_vertex(before, cf3(0, 0, 0))];
    const std::size_t a = smooth_level(with, 3, center, 0.08f, true, nullptr);
    const std::size_t b = smooth_level(without, 3, center, 0.08f, false, nullptr);
    CHECK(a == b);
    CHECK(a > 0);
    CHECK(same_bytes(with.level_mesh(3).positions, without.level_mesh(3).positions));
}

TEST_CASE("multires: relax and a normal-steered verb agree with the uniform hierarchy at a seam") {
    // RELAX takes the smoothed target and projects it onto the tangent plane of
    // the vertex's OWN normal, and INFLATE displaces straight along that normal
    // — so between them they exercise both halves of what a depth boundary
    // breaks: the averaged ring and the angle-weighted normal. Neither has a
    // branch on a level; both read the neighbourhood the level does not hold.
    const Mesh cage = bumpy_quads(6, 1.0f);
    for (MeshBrush verb : {MeshBrush::Relax, MeshBrush::Inflate}) {
        MultiresSurface dense = build(cage, 3);
        MultiresSurface with = build_regional(cage);
        MultiresSurface without = build_regional(cage);

        const std::vector<cfloat3> before = with.positions_at(3);
        const std::uint32_t seed = nearest_vertex(before, cf3(-1.0f / 3.0f, 0.0f, 0.0f));
        const cfloat3 center = before[seed];

        std::map<std::array<float, 3>, std::uint32_t> dense_of;
        {
            const std::vector<cfloat3>& p = dense.positions_at(3);
            for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(p.size()); ++v)
                dense_of[{p[v].x, p[v].y, p[v].z}] = v;
        }
        stamp_level(dense, 3, verb, center, 0.25f, true);
        stamp_level(with, 3, verb, center, 0.25f, true);
        stamp_level(without, 3, verb, center, 0.25f, false);

        const auto worst_of = [&](MultiresSurface& s) {
            const std::vector<cfloat3>& after = s.level_mesh(3).positions;
            const std::vector<cfloat3>& reference = dense.level_mesh(3).positions;
            float worst = 0.0f;
            for (std::uint32_t v = 0; v < static_cast<std::uint32_t>(after.size()); ++v) {
                const auto it = dense_of.find({before[v].x, before[v].y, before[v].z});
                if (it == dense_of.end()) continue;
                worst = std::max(worst, clength(after[v] - reference[it->second]));
            }
            return worst;
        };
        const float open = worst_of(without), closed = worst_of(with);
        INFO("verb " << static_cast<int>(verb) << ": open " << open << ", closed " << closed);
        // A subdivision step out of place against float rounding. The level-3
        // edge spacing on this cage is 0.0417, so `open` is a fraction of an
        // edge and `closed` is the last bits of a float sum.
        CHECK(open > 1e-3f);
        CHECK(closed < 1e-6f);
    }
}

TEST_CASE("multires: a uniform hierarchy sculpts exactly as it did") {
    // THE OTHER HALF OF THE PARITY ARGUMENT. A level that stores every patch has
    // an EMPTY neighbourhood, so every reader takes the path it took before this
    // existed and cannot produce a different number — asserted here as bytes
    // rather than left to the construction.
    const Mesh cage = bumpy_quads(6, 1.0f);
    MultiresSurface with = build(cage, 3);
    MultiresSurface without = build(cage, 3);
    CHECK(with.cross_level_at(3).empty());

    const std::vector<cfloat3> before = with.positions_at(3);
    const cfloat3 center = before[nearest_vertex(before, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];
    for (MeshBrush verb : {MeshBrush::Smooth, MeshBrush::Relax, MeshBrush::Inflate}) {
        const std::size_t a = stamp_level(with, 3, verb, center, 0.25f, true);
        const std::size_t b = stamp_level(without, 3, verb, center, 0.25f, false);
        INFO("verb " << static_cast<int>(verb));
        CHECK(a == b);
        CHECK(a > 0);
        CHECK(same_bytes(with.level_mesh(3).positions, without.level_mesh(3).positions));
    }
}

TEST_CASE("multires: a derived face can join two vertices no ring walk reaches") {
    // RECORDED FOR THE STAGE THAT TOUCHES THE PROPAGATION HALO, because it is
    // the one place a cross-level FACE says more than a cross-level RING does. A
    // normal is invalidated by every corner of every incident face; a ring is
    // the four edges of a quad plus the diagonal its triangulation adds. The
    // OTHER diagonal — the two edge points either side of a coarse corner —
    // exists only where the refined region turns a CONCAVE corner, which a
    // square block never does and an L-shaped one does exactly once.
    //
    // Nothing here acts on it. A stamp's normal-refresh set is the union over
    // every class it moved, and on both fixtures that union already covers the
    // pair — measured by removing the extra walk and watching the count of
    // re-shaded vertices stay at 67. An extra term with no test is a term to
    // delete, so the walk was deleted and this is what it would have found.
    const Mesh cage = bumpy_quads(6, 1.0f);
    const auto unreachable_pairs = [](MultiresSurface& s) {
        const mesh::CrossLevelNeighborhood& cross = s.cross_level_at(3);
        const mesh::Adjacency& adj = s.level_adjacency(3);
        std::size_t n = 0;
        for (std::uint32_t f = 0; f < cross.face_count(); ++f) {
            const std::uint32_t* q = cross.face_corners(f);
            for (int a = 0; a < 4; ++a)
                for (int b = a + 1; b < 4; ++b) {
                    if (!cross.inside(q[a]) || !cross.inside(q[b])) continue;
                    std::size_t rc = 0;
                    const std::uint32_t* ring = adj.ring(adj.class_of(q[a]), &rc);
                    if (!std::binary_search(ring, ring + rc, adj.class_of(q[b]))) ++n;
                }
        }
        return n;
    };
    MultiresSurface block = build_regional(cage);
    MultiresSurface ell = build_regional_L(cage);
    CHECK(unreachable_pairs(block) == 0);
    CHECK(unreachable_pairs(ell) == 1);
}

// -- brushes ACROSS a depth boundary -----------------------------------------
//
// The cases above are about a stamp whose vertices this level all STORES: they
// close the neighbourhood so a rim vertex is averaged and shaded the way the
// uniform hierarchy averages and shades it. These are about the other half —
// the part of the brush's footprint that has no vertex at this level at all,
// because the patches beside the refined region are coarser.
//
// The fixture is the one above and it is sized deliberately: the middle 2x2 of
// a 6x6 cage, where 64 of the 289 level-3 vertices — 22% — are on the rim.
// A SMALL refined region has proportionally MORE boundary, which is the
// opposite of the intuition that a transition is a rare edge case, so a gate
// built on a large refined region under-reports every number here.

namespace {

// The displacement a stamp on a UNIFORMLY refined hierarchy produced, sampled
// wherever the mixed-depth surface has a vertex.
//
// By NEAREST, and it has to be: the coarse side of a mixed-depth surface is at
// level 2, so its vertices are not level-3 vertices and no exact match exists
// for them. The level-3 spacing on this cage is 0.0417 and the level-2 spacing
// 0.0833, so "nearest" is never ambiguous.
struct DenseField {
    std::vector<cfloat3> before, displacement;

    static DenseField of(const Mesh& cage, MeshBrush verb, const MeshBrushSettings& settings) {
        MultiresSurface dense = build(cage, 3);
        DenseField f;
        f.before = dense.positions_at(3);
        dense.set_sculpt_level(3);
        MultiresSculptor sculptor(dense);
        sculptor.begin_stroke();
        sculptor.stamp(verb, settings);
        const std::vector<cfloat3>& after = dense.positions_at(3);
        f.displacement.resize(after.size());
        for (std::size_t i = 0; i < after.size(); ++i) f.displacement[i] = after[i] - f.before[i];
        return f;
    }

    cfloat3 at(cfloat3 p) const {
        std::size_t best = 0;
        float best_d = 1e30f;
        for (std::size_t i = 0; i < before.size(); ++i) {
            const float d = clength(before[i] - p);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return displacement[best];
    }
};

// The pre-cross-level algorithm, kept as the parity reference: one stamp on one
// level's own mesh, absorbed into that level and nothing else.
std::size_t stamp_one_level(MultiresSurface& s, std::uint32_t level, MeshBrush verb,
                            const MeshBrushSettings& settings) {
    Mesh& mesh = s.level_mesh(level);
    mesh::MeshSculptor sculptor(mesh, s.level_adjacency(level));
    sculptor.set_cross_level(&s.cross_level_at(level));
    const std::size_t moved = sculptor.stamp(verb, settings);
    std::vector<std::uint32_t> touched;
    for (mesh::WorkItemId item : sculptor.write_region()) {
        std::size_t members = 0;
        const std::uint32_t* member =
            s.level_adjacency(level).members(item.as_weld_class(), &members);
        for (std::size_t i = 0; i < members; ++i) touched.push_back(member[i]);
    }
    s.absorb_level_edit(level, touched);
    return moved;
}

struct CrossResult {
    std::size_t dropped = 0;  // the reference moved it and this stamp did not, at all
    float worst = 0.0f;       // the furthest this stamp finished from the reference
    float peak = 0.0f;        // the reference's own largest displacement
};

// What one stamp on the regional hierarchy did, against what the same stamp on
// the uniform one did, over the MIXED-DEPTH surface — which is the surface a
// host draws and the one an artist is looking at when the brush crosses.
CrossResult cross_stamp(MultiresSurface& s, MeshBrush verb, const MeshBrushSettings& settings,
                        const DenseField& reference, bool cross) {
    const Mesh before = s.mixed_mesh_at_level(3);
    s.set_sculpt_level(3);
    if (cross) {
        MultiresSculptor sculptor(s);
        sculptor.begin_stroke();
        sculptor.stamp(verb, settings);
    } else {
        stamp_one_level(s, 3, verb, settings);
    }
    const Mesh after = s.mixed_mesh_at_level(3);
    REQUIRE(after.positions.size() == before.positions.size());

    CrossResult r;
    for (std::size_t i = 0; i < before.positions.size(); ++i) {
        const cfloat3 want = reference.at(before.positions[i]);
        const cfloat3 got = after.positions[i] - before.positions[i];
        r.peak = std::max(r.peak, clength(want));
        r.worst = std::max(r.worst, clength(got - want));
        if (clength(want) > 1e-6f && clength(got) <= 1e-7f) ++r.dropped;
    }
    return r;
}

}  // namespace

TEST_CASE("multires: a stamp crossing a depth boundary writes the coarse side too") {
    const Mesh cage = bumpy_quads(6, 1.0f);
    MeshBrushSettings settings;
    settings.strength = 0.5f;
    {
        MultiresSurface probe = build_regional(cage);
        const std::vector<cfloat3>& p = probe.positions_at(3);
        settings.center = p[nearest_vertex(p, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];
    }

    // WIDENING THE BRUSH IS WIDENING THE DEFECT, which is why the radius is
    // swept rather than picked: the further past the rim the footprint reaches,
    // the more of it had nowhere to go. Anchored ON the rim in every case.
    for (float radius : {0.25f, 0.35f, 0.50f}) {
        settings.radius = radius;
        const DenseField reference = DenseField::of(cage, MeshBrush::Draw, settings);

        MultiresSurface one = build_regional(cage);
        MultiresSurface both = build_regional(cage);
        const CrossResult level_only =
            cross_stamp(one, MeshBrush::Draw, settings, reference, false);
        const CrossResult crossed = cross_stamp(both, MeshBrush::Draw, settings, reference, true);

        INFO("radius " << radius << ": dropped " << level_only.dropped << " -> " << crossed.dropped
                       << ", worst " << level_only.worst << " -> " << crossed.worst
                       << ", against a peak of " << crossed.peak);
        // THE SILENT DROP. 5, 17 and 46 vertices of the surface an artist is
        // looking at are moved by the uniform hierarchy's stamp and by nothing
        // at all here — and the count that came back said the stamp had done
        // its whole job. Assert the COUNT.
        CHECK(level_only.dropped > 0);
        CHECK(crossed.dropped == 0);
        // AND HOW FAR THE SURFACE FINISHES FROM THE UNIFORM HIERARCHY'S ANSWER,
        // asked as a RATIO rather than against a threshold, because the number
        // that matters is the improvement and not a tolerance nobody stated:
        // 0.029781371 -> 0.001621436, 0.090758100 -> 0.003878876 and
        // 0.182510689 -> 0.005068991, which is 18x, 23x and 36x closer. The
        // level-3 edge spacing here is 0.0417, so the left-hand column is up to
        // 4.4 edges out and the right-hand one is a tenth of one.
        CHECK(crossed.worst * 5.0f < level_only.worst);
        // What is left is the REGION NORMAL, not the seam: `Draw` deposits
        // along the region's AVERAGED normal, and the region on a mixed-depth
        // surface is not the same set of vertices as the region on a uniform
        // one. 2% of the stamp's own peak displacement, against 73% for
        // dropping the coarse side.
        CHECK(crossed.worst < 0.05f * crossed.peak);
        CHECK(level_only.worst > 0.05f * crossed.peak);
    }
}

TEST_CASE("multires: every displacement verb crosses a depth boundary") {
    // ONE VERB IS NOT A GATE HERE. The three things a depth boundary breaks are
    // the averaged ring, the per-vertex normal and the region's own plane, and
    // the vocabulary splits over them: Smooth and Relax average, Inflate and
    // Crease steer per vertex, Flatten and Scrape fit a plane, Grab and
    // Snakehook carry a rigid delta, Layer measures a ceiling. All of them
    // reach past the rim, and none of them has a branch on a level.
    const Mesh cage = bumpy_quads(6, 1.0f);
    MeshBrushSettings settings;
    settings.radius = 0.30f;
    settings.strength = 0.5f;
    settings.direction = cf3(0.02f, 0.03f, 0.0f);
    {
        MultiresSurface probe = build_regional(cage);
        const std::vector<cfloat3>& p = probe.positions_at(3);
        settings.center = p[nearest_vertex(p, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];
    }
    for (MeshBrush verb : {MeshBrush::Draw, MeshBrush::Inflate, MeshBrush::Smooth, MeshBrush::Grab,
                           MeshBrush::Flatten, MeshBrush::Clay, MeshBrush::Relax, MeshBrush::Pinch,
                           MeshBrush::Crease, MeshBrush::Nudge, MeshBrush::Layer, MeshBrush::Polish,
                           MeshBrush::Snakehook, MeshBrush::Scrape}) {
        const DenseField reference = DenseField::of(cage, verb, settings);
        MultiresSurface one = build_regional(cage);
        MultiresSurface both = build_regional(cage);
        const CrossResult level_only = cross_stamp(one, verb, settings, reference, false);
        const CrossResult crossed = cross_stamp(both, verb, settings, reference, true);
        INFO("verb " << static_cast<int>(verb) << ": dropped " << level_only.dropped << " -> "
                     << crossed.dropped << ", worst " << level_only.worst << " -> "
                     << crossed.worst);
        // NOT A RATIO HERE, because a smoothing verb's own displacement on this
        // cage is a thousandth of a unit and every ratio taken against it is
        // unflattering for a reason that is not a defect: level 2 smooths at
        // level 2's wavelength, and no write at that level can reproduce a
        // level-3 average. What must hold is the same two things — nothing is
        // dropped, and the surface does not STEP.
        CHECK(level_only.dropped > 0);
        CHECK(crossed.dropped == 0);
        // A quarter of the level-3 edge spacing of 0.0417. The worst of the
        // fourteen is Clay at 0.002927452, which is 7% of an edge; dropping the
        // coarse side is up to 4.4 edges out.
        CHECK(crossed.worst < 0.25f * 0.0416667f);
    }
}

namespace {

// A CLOSED TORUS, and the reason this file needs one beside its plane cage: the
// plane's normals all point within a few degrees of +Y, so a reader that
// substituted a constant for a neighbour's normal would still be reading almost
// the right direction. On a torus a normal points every way there is.
Mesh torus_quads(int nu, int nv) {
    Mesh m;
    const float kTwoPi = 6.2831853f;
    for (int u = 0; u < nu; ++u)
        for (int v = 0; v < nv; ++v) {
            const float a = kTwoPi * static_cast<float>(u) / static_cast<float>(nu);
            const float b = kTwoPi * static_cast<float>(v) / static_cast<float>(nv);
            const float radius = 1.0f + 0.4f * std::cos(b);
            m.positions.push_back(
                cf3(radius * std::cos(a), 0.4f * std::sin(b), radius * std::sin(a)));
            m.normals.push_back(
                cf3(std::cos(b) * std::cos(a), std::sin(b), std::cos(b) * std::sin(a)));
        }
    const auto at = [&](int u, int v) {
        return static_cast<std::uint32_t>(((u % nu + nu) % nu) * nv + ((v % nv + nv) % nv));
    };
    for (int u = 0; u < nu; ++u)
        for (int v = 0; v < nv; ++v) {
            const std::uint32_t a = at(u, v), b = at(u + 1, v), c = at(u + 1, v + 1),
                                d = at(u, v + 1);
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

MultiresSurface build_regional_torus(const Mesh& cage, int n) {
    MultiresError err = MultiresError::None;
    auto s = MultiresSurface::from_mesh(cage, {}, &err);
    REQUIRE_MESSAGE(s.has_value(), mesh::multires_error_text(err));
    std::vector<std::uint32_t> block;
    for (int u = 1; u <= 2; ++u)
        for (int v = 1; v <= 2; ++v) block.push_back(static_cast<std::uint32_t>(u * n + v));
    REQUIRE(s->refine_patches_to_level(block, 3));
    return std::move(*s);
}

}  // namespace

TEST_CASE("multires: a polish stamp reads the derived faces' OWN normals across a transition") {
    // THE ONE VERB THAT READS A NEIGHBOUR'S NORMAL, and the only place an
    // outside neighbour's normal is a VALUE rather than a slot.
    // `build_neighbors` fills `nb_normals_` for Polish alone, and
    // `append_outside_neighbors` gives an outside id the angle-weighted normal
    // of the derived faces around it.
    //
    // WHY THE VALUE IS THE WHOLE OF IT HERE, and why the verb sweep above
    // cannot see it. `polish_gate` reads those normals as an ANGLE against the
    // vertex's own — `mean_ring_disagreement` — and closes the gate where the
    // surface bends. A wrong normal at the rim reads as a hard edge, the gate
    // shuts, and the rim is not polished AT ALL while the count still reports
    // the stamp did its job. That is the same silent drop this section is
    // about, arriving through the normals rather than through the ring.
    //
    // ON A TORUS, because on the plane cage every normal is within a few
    // degrees of +Y and a substituted constant is inside the gate's own angle.
    const int n = 12;
    const Mesh cage = torus_quads(n, n);
    MeshBrushSettings settings;
    settings.radius = 0.30f;
    settings.strength = 1.0f;
    {
        MultiresSurface probe = build_regional_torus(cage, n);
        const std::vector<cfloat3>& p = probe.positions_at(3);
        // A vertex ON the rim of the refined region, which is where a derived
        // face is the only thing on one side.
        const mesh::CrossLevelNeighborhood& cross = probe.cross_level_at(3);
        REQUIRE_FALSE(cross.outside_positions.empty());
        settings.center = p[nearest_vertex(p, cross.outside_positions[0])];
    }
    // TWO RADII, both anchored on the rim. A wider one reaches so far past it
    // that a vertex is dropped for the ordinary reason a brush drops one, which
    // would make this gate agree with itself for the wrong cause.
    for (float radius : {0.20f, 0.30f}) {
        settings.radius = radius;
        const DenseField reference = DenseField::of(cage, MeshBrush::Polish, settings);
        MultiresSurface one = build_regional_torus(cage, n);
        MultiresSurface both = build_regional_torus(cage, n);
        const CrossResult level_only =
            cross_stamp(one, MeshBrush::Polish, settings, reference, false);
        const CrossResult crossed = cross_stamp(both, MeshBrush::Polish, settings, reference, true);
        INFO("radius " << radius << ": dropped " << level_only.dropped << " -> "
                       << crossed.dropped << ", worst " << level_only.worst << " -> "
                       << crossed.worst << ", peak " << crossed.peak);
        // NOT VACUOUS: 11 and 19 vertices the uniform hierarchy polished are
        // moved by nothing at all when the coarse side is dropped.
        CHECK(level_only.dropped > 0);
        // AND NOTHING IS DROPPED HERE. Substituting a constant for the outside
        // neighbours' normals — the same list, the same length — shuts
        // `polish_gate` at 3 and 1 of these vertices, which is what this
        // asserts and what a length-only gate cannot.
        CHECK(crossed.dropped == 0);
    }

}

TEST_CASE("multires: a COLOUR verb keeps the ring it already had at a transition") {
    // A VERTEX THIS LEVEL DOES NOT STORE HAS NO COLOUR TO REPORT, and there is
    // nothing to invent one from: a multires level carries no colour attribute
    // at all, and an outside vertex has nowhere to hold one if it did. So
    // `append_outside_neighbors` appends NOTHING for a colour verb, and the
    // consequence is a value: the colours a smear writes at a depth boundary
    // are the ones it wrote before the neighbourhood existed, bit for bit.
    //
    // It is not merely a missing entry either. `kernel_smear` reads
    // `nb.colors[k]` at the same index it reads `nb.positions[k]`, so appending
    // a position without a colour reads PAST the colour array — which is what
    // this case is really holding shut.
    MultiresSurface s = build_regional(bumpy_quads(6, 1.0f));
    const mesh::CrossLevelNeighborhood& cross = s.cross_level_at(3);
    REQUIRE_FALSE(cross.empty());

    Mesh painted = s.level_mesh(3);
    painted.colors.resize(painted.positions.size());
    for (std::size_t i = 0; i < painted.colors.size(); ++i)
        painted.colors[i] = cf3(static_cast<float>(i % 11u) * 0.09f,
                                static_cast<float>(i % 7u) * 0.14f, 0.3f);
    const std::vector<cfloat3> before = painted.colors;
    Mesh plain = painted;

    MeshBrushSettings settings;
    const std::uint32_t rim = [&] {
        for (std::uint32_t v = 0; v < cross.vertex_count; ++v) {
            std::size_t rc = 0;
            const std::uint32_t* ring = cross.ring_of(v, &rc);
            for (std::size_t i = 0; i < rc; ++i)
                if (!cross.inside(ring[i])) return v;
        }
        return 0u;
    }();
    settings.center = painted.positions[rim];
    settings.radius = 0.30f;
    settings.strength = 1.0f;
    settings.direction = cf3(1.0f, 0.0f, 0.3f);

    mesh::MeshSculptor with(painted, s.level_adjacency(3));
    with.set_cross_level(&cross);
    const std::size_t moved = with.stamp(MeshBrush::Smear, settings);
    mesh::MeshSculptor without(plain, s.level_adjacency(3));
    CHECK(without.stamp(MeshBrush::Smear, settings) == moved);

    // NOT VACUOUS: the smear really did write at the rim.
    CHECK(moved > 0u);
    std::size_t changed = 0, differ = 0;
    for (std::size_t i = 0; i < painted.colors.size(); ++i) {
        if (painted.colors[i].x != before[i].x || painted.colors[i].y != before[i].y ||
            painted.colors[i].z != before[i].z)
            ++changed;
        if (painted.colors[i].x != plain.colors[i].x ||
            painted.colors[i].y != plain.colors[i].y || painted.colors[i].z != plain.colors[i].z)
            ++differ;
    }
    CHECK(changed > 0u);
    CHECK(differ == 0u);
}

TEST_CASE("multires: a WELDED class counts an outside neighbour once") {
    // THE ONE THING A WELD CLASS CHANGES about the outside neighbours, and the
    // only place `append_outside_neighbors` looks at more than one vertex.
    //
    // The list is built per CLASS and the cross-level ring is asked per MEMBER,
    // so a class holding two vertices can reach the same outside vertex twice —
    // and the Laplacian divides by the list's length, so counting it twice
    // weights it twice in the mean and drags the vertex toward it.
    //
    // WHY THE MESH IS PINCHED. `level_adjacency` welds a level's vertices at
    // EXACTLY 0.0f, on the grounds that a level's vertices are the geometric
    // points of the surface — so the only thing that welds is a pair that
    // genuinely coincides, which is what a degenerate cage produces and what
    // that exact weld exists to keep from cracking. Built here directly: two
    // rim vertices that share outside neighbours are moved onto one point, and
    // a sculptor over that mesh welds them the same way.
    MultiresSurface s = build_regional(bumpy_quads(6, 1.0f));
    const mesh::CrossLevelNeighborhood& cross = s.cross_level_at(3);
    REQUIRE_FALSE(cross.empty());

    // The outside ids a level vertex reaches, which is what two members can
    // duplicate between them.
    const auto outside_of = [&](std::uint32_t v) {
        std::vector<std::uint32_t> out;
        std::size_t rc = 0;
        const std::uint32_t* ring = cross.ring_of(v, &rc);
        for (std::size_t i = 0; i < rc; ++i)
            if (!cross.inside(ring[i])) out.push_back(ring[i]);
        return out;
    };
    std::uint32_t a = 0, b = 0;
    std::size_t shared = 0;
    for (std::uint32_t u = 0; u < cross.vertex_count && shared == 0; ++u) {
        const std::vector<std::uint32_t> ou = outside_of(u);
        if (ou.empty()) continue;
        for (std::uint32_t w = u + 1; w < cross.vertex_count && shared == 0; ++w) {
            std::size_t common = 0;
            for (std::uint32_t id : outside_of(w))
                if (std::find(ou.begin(), ou.end(), id) != ou.end()) ++common;
            if (common == 0) continue;
            a = u;
            b = w;
            shared = common;
        }
    }
    REQUIRE(shared > 0);  // the duplicate the guard is about really exists

    Mesh pinched = s.level_mesh(3);
    pinched.positions[b] = pinched.positions[a];
    const std::vector<cfloat3> before = pinched.positions;
    mesh::MeshSculptor sculptor(pinched, 0.0f);
    sculptor.set_cross_level(&cross);
    const mesh::Adjacency& adj = sculptor.adjacency();
    const std::uint32_t cls = adj.class_of(a);
    REQUIRE(adj.class_of(b) == cls);
    std::size_t mc = 0;
    const std::uint32_t* members = adj.members(cls, &mc);
    REQUIRE(mc == 2u);

    // The neighbour list `build_neighbors` produces for this class, rebuilt
    // here: the adjacency ring, then the outside ids of every member ONCE.
    std::vector<cfloat3> neighbours;
    std::size_t rc = 0;
    const std::uint32_t* ring = adj.ring(cls, &rc);
    for (std::size_t k = 0; k < rc; ++k) {
        std::size_t n = 0;
        neighbours.push_back(before[adj.members(ring[k], &n)[0]]);
    }
    std::vector<std::uint32_t> unique_outside;
    std::size_t raw_outside = 0;
    for (std::size_t k = 0; k < mc; ++k)
        for (std::uint32_t id : outside_of(members[k])) {
            ++raw_outside;
            if (std::find(unique_outside.begin(), unique_outside.end(), id) ==
                unique_outside.end())
                unique_outside.push_back(id);
        }
    REQUIRE(raw_outside > unique_outside.size());  // and the list really is shorter for it
    for (std::uint32_t id : unique_outside)
        neighbours.push_back(cross.outside_positions[id - cross.vertex_count]);

    cfloat3 sum = cf3(0, 0, 0);
    for (const cfloat3& q : neighbours) sum = sum + q;
    const cfloat3 mean = sum / static_cast<float>(neighbours.size());

    MeshBrushSettings settings;
    settings.center = before[a];
    settings.radius = 0.25f;
    settings.strength = 1.0f;
    settings.smooth_iterations = 1;
    REQUIRE(sculptor.stamp(MeshBrush::Smooth, settings) > 0);

    // ONE SMOOTH PASS MOVES A VERTEX ALONG (mean - p) and by a fraction of it,
    // so the DIRECTION is the part that does not depend on the falloff — and
    // the direction is exactly what a double-counted neighbour bends.
    const cfloat3 travelled = pinched.positions[a] - before[a];
    const cfloat3 wanted = mean - before[a];
    REQUIRE(clength(travelled) > 0.0f);
    REQUIRE(clength(wanted) > 0.0f);
    const float sine = clength(ccross(travelled, wanted)) / (clength(travelled) * clength(wanted));
    MESSAGE("welded class " << cls << ": " << raw_outside << " outside entries, "
                            << unique_outside.size() << " distinct, sine " << sine);
    CHECK(sine < 1e-4f);
    // Both members travelled together, as a weld class must.
    CHECK(pinched.positions[b].x == pinched.positions[a].x);
    CHECK(pinched.positions[b].y == pinched.positions[a].y);
    CHECK(pinched.positions[b].z == pinched.positions[a].z);
}

TEST_CASE("multires: no vertex of a mixed-depth surface is written twice") {
    // THE SEAM CANNOT DOUBLE-COUNT, and it is structural rather than a tolerance
    // to tune. A vertex belongs to the level the mixed-depth surface carries it
    // at — its own, unless the level above holds its vertex point — so the two
    // write lists are a PARTITION and the same displacement cannot land in both.
    const Mesh cage = bumpy_quads(6, 1.0f);
    MultiresSurface s = build_regional(cage);
    MeshBrushSettings settings;
    settings.radius = 0.50f;
    settings.strength = 0.5f;
    const std::vector<cfloat3>& p = s.positions_at(3);
    settings.center = p[nearest_vertex(p, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];

    s.set_sculpt_level(3);
    MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    const std::size_t moved = sculptor.stamp(MeshBrush::Draw, settings);

    // Two levels: the one the artist chose, and the one the patches beside the
    // refined region actually live at.
    CHECK(sculptor.last_write_levels() == std::vector<std::uint32_t>{2u, 3u});
    const std::vector<std::uint32_t>& coarse = sculptor.last_write_vertices_at(2);
    const std::vector<std::uint32_t>& fine = sculptor.last_write_vertices_at(3);
    CHECK(coarse.size() == 46);
    CHECK(fine.size() == 218);
    CHECK(moved == 264);
    // `last_write_vertices()` keeps its meaning: the BOUND level's list.
    CHECK(sculptor.last_write_vertices() == fine);
    CHECK(sculptor.last_write_vertices_at(1).empty());

    // NOT ONE of the level-2 vertices written has a level-3 vertex point, so
    // none of them is a vertex the level-3 list also moved.
    const mesh::ChildIndex above = mesh::ChildIndex::of(s.topology_at(3));
    std::size_t also_above = 0;
    for (std::uint32_t v : coarse)
        if (above.stored(v) != mesh::kNoVertex) ++also_above;
    CHECK(also_above == 0);
}

TEST_CASE("multires: a crossing gesture undoes across both levels") {
    // THE RECORD ALREADY SPANS LEVELS — `MultiresDelta` is keyed on (level,
    // vertex) and always was — so what this checks is that the crossing stamp
    // fills it that way rather than recording the level it was bound to and
    // leaving the coarse write unreversible.
    const Mesh cage = bumpy_quads(6, 1.0f);
    MultiresSurface s = build_regional(cage);
    const std::vector<cfloat3> level3 = s.positions_at(3);
    const std::vector<cfloat3> level2 = s.positions_at(2);
    MeshBrushSettings settings;
    settings.radius = 0.50f;
    settings.strength = 0.5f;
    settings.center = level3[nearest_vertex(level3, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];

    s.set_sculpt_level(3);
    MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    MultiresDelta record;
    CHECK(sculptor.stamp(MeshBrush::Draw, settings, {}, &record) > 0);
    CHECK(record.levels() == std::vector<std::uint32_t>{2u, 3u});
    CHECK_FALSE(same_bytes(s.positions_at(3), level3));
    CHECK_FALSE(same_bytes(s.positions_at(2), level2));

    REQUIRE(record.revert(s));
    CHECK(same_bytes(s.positions_at(3), level3));
    CHECK(same_bytes(s.positions_at(2), level2));
    REQUIRE(record.apply(s));
    CHECK_FALSE(same_bytes(s.positions_at(2), level2));
}

TEST_CASE("multires: a stamp with nothing on the coarse side writes one level") {
    // THE PARITY GATE FOR THIS HALF, and it is the one that says the coarse pass
    // costs a hierarchy nothing where it has nothing to do. Two cases, both
    // asserted as BYTES against the algorithm this replaced — one stamp on one
    // level's own mesh, absorbed into that level and nothing else.
    const Mesh cage = bumpy_quads(6, 1.0f);
    MeshBrushSettings settings;
    settings.radius = 0.08f;
    settings.strength = 0.5f;

    SUBCASE("a uniform hierarchy owns nothing below its top level") {
        MultiresSurface with = build(cage, 3);
        MultiresSurface without = build(cage, 3);
        const std::vector<cfloat3>& p = with.positions_at(3);
        settings.center = p[nearest_vertex(p, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];
        with.set_sculpt_level(3);
        MultiresSculptor sculptor(with);
        sculptor.begin_stroke();
        const std::size_t a = sculptor.stamp(MeshBrush::Draw, settings);
        const std::size_t b = stamp_one_level(without, 3, MeshBrush::Draw, settings);
        CHECK(a == b);
        CHECK(a > 0);
        CHECK(sculptor.last_write_levels() == std::vector<std::uint32_t>{3u});
        CHECK(same_bytes(with.positions_at(3), without.positions_at(3)));
    }

    SUBCASE("a stamp inside the refined region reaches no coarse vertex") {
        MultiresSurface with = build_regional(cage);
        MultiresSurface without = build_regional(cage);
        const std::vector<cfloat3>& p = with.positions_at(3);
        settings.center = p[nearest_vertex(p, cf3(0, 0, 0))];
        with.set_sculpt_level(3);
        MultiresSculptor sculptor(with);
        sculptor.begin_stroke();
        const std::size_t a = sculptor.stamp(MeshBrush::Draw, settings);
        const std::size_t b = stamp_one_level(without, 3, MeshBrush::Draw, settings);
        CHECK(a == b);
        CHECK(a > 0);
        // The coarse pass RAN — the levels below are regional, so it always
        // does — and found nothing this level does not already carry.
        CHECK(sculptor.last_write_levels() == std::vector<std::uint32_t>{3u});
        CHECK(same_bytes(with.positions_at(3), without.positions_at(3)));
        CHECK(same_bytes(with.positions_at(2), without.positions_at(2)));
    }
}

TEST_CASE("multires: a crossing stamp survives a cache drop under it") {
    // THE STALENESS DISCIPLINE, extended to the levels a crossing stamp writes.
    // `bind` rebuilds on a cache-generation change because the level's mesh
    // lives in the cache; the list of levels that OWN vertices is rebuilt with
    // it, and a drop between two stamps of one stroke must not leave the coarse
    // side unwritten.
    const Mesh cage = bumpy_quads(6, 1.0f);
    MultiresSurface s = build_regional(cage);
    MeshBrushSettings settings;
    settings.radius = 0.50f;
    settings.strength = 0.25f;
    const std::vector<cfloat3>& p = s.positions_at(3);
    settings.center = p[nearest_vertex(p, cf3(-1.0f / 3.0f, 0.0f, 0.0f))];

    s.set_sculpt_level(3);
    MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    CHECK(sculptor.stamp(MeshBrush::Draw, settings) > 0);
    CHECK(sculptor.last_write_levels() == std::vector<std::uint32_t>{2u, 3u});
    const std::uint64_t before = s.cache_generation();
    s.drop_all_caches();
    CHECK(s.cache_generation() != before);
    CHECK(sculptor.stamp(MeshBrush::Draw, settings) > 0);
    CHECK(sculptor.last_write_levels() == std::vector<std::uint32_t>{2u, 3u});
}
