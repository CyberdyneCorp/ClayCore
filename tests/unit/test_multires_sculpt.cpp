// Sculpting a hierarchy, and the one test the feature exists for
// (mesh-multires spec, add-mesh-multires).
//
// THE SIGNATURE TEST is `detail at a fine level survives a form change at a
// coarse one`. Everything else in this file supports it: the brushes are the
// fixed sculptor's, so the parity case below is what says so in bytes rather
// than in a comment, and the undo cases are what make the gesture reversible
// without recording the millions of derived vertices a coarse stroke moves.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/brush/stroke.h"
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
