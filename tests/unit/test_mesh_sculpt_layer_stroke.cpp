// THE LAYERED STROKE TRANSACTION (mesh-sculpt-layers spec,
// add-mesh-sculpt-layers, tasks 3.5, 4.2-4.8, 6.1, 6.2).
//
// `test_mesh_sculpt_layers.cpp` holds the STACK's semantics — what a strength
// means, what commutes, what a merge leaves. This file holds the GESTURE's, and
// the two are separated because the transaction exists for three reasons the
// stack cannot state:
//
//   1. THE TARGET IS PINNED AT `begin`. A host that changes the active layer
//      between two dabs must not split one gesture across two channels, so the
//      case here changes it mid-stroke and checks that every dab still landed
//      in one — which is also requirement 3.5, because a mirrored stamp is
//      another stamp of the same transaction and nothing else.
//   2. THE COMPOSITION IS HELD. Every slider refuses while a stroke is open,
//      and a rename does not — the split is between "moves a vertex" and "does
//      not", and it is asserted rather than described.
//   3. CANCEL IS EXACT. A layered write is `L += ΔE`, so subtracting the deltas
//      back off would leave the last bits of every touched coefficient
//      somewhere else. The cases here compare CHECKSUMS, which is the only
//      comparison that can tell an exact restore from a close one: two
//      different coefficient sets evaluate to the same rounded position.
//
// The verbs that could not exist before a hierarchy stored form and detail
// apart get the same treatment. `erase`, `restore` and the three smoothing
// modes are each asserted by WHAT THEY LEFT ALONE — the base checksum, the
// other layer's checksum — because every one of them would look right in a
// render while having flattened something.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/layered_sculpt.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt_layer.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DetailStampMode;
using mesh::DetailStampSettings;
using mesh::LayeredMultiresSculptor;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresSmoothMode;
using mesh::MultiresSurface;
using mesh::MultiresWriteDomain;
using mesh::SculptLayerDelta;
using mesh::SculptLayerId;

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
            m.quads.insert(m.quads.end(), {a, a + 1, a + stride + 1, a + stride});
            m.indices.insert(m.indices.end(),
                             {a, a + 1, a + stride + 1, a, a + stride + 1, a + stride});
        }
    return m;
}

MultiresSurface build(int levels) {
    auto surface = MultiresSurface::from_mesh(plane_quads(4, 1.0f));
    REQUIRE(surface.has_value());
    for (int i = 0; i < levels; ++i) REQUIRE(surface->add_level());
    REQUIRE(surface->set_sculpt_level(static_cast<std::uint32_t>(levels)));
    return std::move(*surface);
}

bool bit_equal(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

MeshBrushSettings dab(cfloat3 centre, float radius = 0.35f, float strength = 0.4f) {
    MeshBrushSettings s;
    s.center = centre;
    s.radius = radius;
    s.strength = strength;
    return s;
}

// The level vertex nearest a point, so a case can name a place rather than an
// index — the indices a level hands out are a subdivision's business.
std::uint32_t nearest(MultiresSurface& surface, std::uint32_t level, cfloat3 point) {
    const std::vector<cfloat3>& p = surface.positions_at(level);
    std::uint32_t best = 0;
    float best_d = 1e30f;
    for (std::uint32_t v = 0; v < p.size(); ++v) {
        const float d = clength(p[v] - point);
        if (d < best_d) {
            best_d = d;
            best = v;
        }
    }
    return best;
}

// How far the layer's stored coefficients stand off zero, summed. What "the
// pass is still there" means when a position comparison cannot tell a preserved
// pass from a re-derived one.
double layer_energy(const MultiresSurface& surface, SculptLayerId id, std::uint32_t level) {
    const mesh::DetailField* field = surface.sculpt_layers().detail_at(id, level);
    if (!field) return 0.0;
    double total = 0.0;
    for (std::uint32_t v = 0; v < field->vertex_count(); ++v) {
        const LocalDetail d = field->get(v);
        total += std::abs(d.tangent) + std::abs(d.bitangent) + std::abs(d.normal);
    }
    return total;
}

}  // namespace

TEST_CASE("cancel restores the channel exactly, and commit hands over one record") {
    MultiresSurface surface = build(2);
    surface.set_detail(2, 40, LocalDetail{0.0f, 0.0f, 0.05f});
    const SculptLayerId id = surface.add_sculpt_layer("pass");
    REQUIRE(surface.set_active_sculpt_layer(id));

    // A pass already on the layer, so cancel has something to restore TO rather
    // than merely emptying a field — restoring an empty layer to empty is a
    // weaker claim than restoring a half-finished one to where it was.
    LayeredMultiresSculptor first(surface);
    REQUIRE(first.begin());
    first.stamp(MeshBrush::Draw, dab(surface.positions_at(2)[40], 0.4f, 0.5f));
    REQUIRE(first.commit());

    const std::uint64_t layer_before = surface.sculpt_layer_checksum();
    const std::uint64_t base_before = surface.detail_checksum();
    const std::vector<cfloat3> shape_before = surface.positions_at(2);

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    CHECK(stroke.target_layer() == id);
    for (int i = 0; i < 6; ++i)
        stroke.stamp(MeshBrush::Draw, dab(cf3(-0.2f + 0.08f * static_cast<float>(i), 0.0f, 0.0f)));
    CHECK(stroke.stamps() == 6);
    CHECK(stroke.record_size() > 0);
    CHECK(surface.sculpt_layer_checksum() != layer_before);

    SUBCASE("cancel puts the recorded values back, to the bit") {
        stroke.cancel();
        CHECK(surface.sculpt_layer_checksum() == layer_before);
        CHECK(surface.detail_checksum() == base_before);
        CHECK(bit_equal(shape_before, surface.positions_at(2)));
        CHECK_FALSE(surface.sculpt_layer_composition_held());
    }
    SUBCASE("commit hands over ONE delta for the whole gesture") {
        SculptLayerDelta record;
        const std::size_t entries = stroke.record_size();
        REQUIRE(stroke.commit(&record));
        CHECK(record.layer() == id);
        CHECK(record.size() == entries);
        CHECK(record.levels() == std::vector<std::uint32_t>{2});
        // And that ONE delta reverses the whole six-stamp gesture, which is
        // what makes it one undo step rather than six.
        REQUIRE(surface.apply_sculpt_layer_delta(record, false));
        CHECK(surface.sculpt_layer_checksum() == layer_before);
        REQUIRE(surface.apply_sculpt_layer_delta(record, true));
        CHECK_FALSE(surface.sculpt_layer_checksum() == layer_before);
    }
    SUBCASE("and cancelling into a FRESH layer restores it to empty, not to sized-and-zero") {
        // THE SHAPE THAT FOUND THE BUG, and the reason it is a separate case
        // from the one above: restoring to values that are already there
        // exercises nothing about a field that did not exist. Undo writes the
        // recorded `before` values, which for an untouched layer are zeros, and
        // leaves the block ALLOCATED until `compact_sculpt_layers` releases it.
        // `sculpt_layer_checksum` was folding the vertex count each lazily-sized
        // field had been sized to, so an exact restore hashed differently from
        // never having written — which makes the change's own gate unprovable
        // and has a host comparing checksums re-uploading forever.
        MultiresSurface fresh = build(2);
        const SculptLayerId untouched = fresh.add_sculpt_layer("fresh");
        REQUIRE(fresh.set_active_sculpt_layer(untouched));
        const std::uint64_t empty = fresh.sculpt_layer_checksum();

        LayeredMultiresSculptor gesture(fresh);
        REQUIRE(gesture.begin());
        CHECK(gesture.stamp(MeshBrush::Draw, dab(fresh.positions_at(2)[40], 0.4f, 0.5f)) > 0);
        CHECK(fresh.sculpt_layer_checksum() != empty);
        gesture.cancel();
        CHECK(fresh.sculpt_layer_checksum() == empty);
        // The storage is still there — that is a memory question, and the one
        // lever for it — while the CONTENT is gone.
        fresh.compact_sculpt_layers();
        CHECK(fresh.sculpt_layer_checksum() == empty);
    }
    SUBCASE("a destroyed transaction discards rather than leaving the lock held") {
        {
            LayeredMultiresSculptor abandoned(surface);
            REQUIRE(abandoned.begin());
            abandoned.stamp(MeshBrush::Draw, dab(cf3(0.4f, 0.0f, 0.4f)));
        }
        CHECK_FALSE(surface.sculpt_layer_composition_held());
    }
    stroke.cancel();
}

TEST_CASE("a hundred stamps over one vertex coalesce to one entry per vertex") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("pass");
    REQUIRE(surface.set_active_sculpt_layer(id));

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    // A radius small enough to reach a handful of vertices, stamped a hundred
    // times in exactly one place. The record must follow the VERTICES, not the
    // stamps.
    const cfloat3 centre = surface.positions_at(2)[40];
    for (int i = 0; i < 100; ++i) stroke.stamp(MeshBrush::Draw, dab(centre, 0.12f, 0.05f));
    CHECK(stroke.stamps() == 100);
    const std::size_t entries = stroke.record_size();
    CHECK(entries > 0);
    CHECK(entries < 100);

    SculptLayerDelta record;
    REQUIRE(stroke.commit(&record));
    CHECK(record.size() == entries);
    // Undo takes back all hundred stamps, because the entry it kept holds where
    // the STROKE found the vertex rather than where the ninety-ninth stamp did.
    REQUIRE(surface.apply_sculpt_layer_delta(record, false));
    CHECK(layer_energy(surface, id, 2) == doctest::Approx(0.0));
}

TEST_CASE("one gesture enters one channel, whatever the host does to the active layer") {
    MultiresSurface surface = build(2);
    const SculptLayerId target = surface.add_sculpt_layer("target");
    const SculptLayerId other = surface.add_sculpt_layer("other");
    REQUIRE(surface.set_active_sculpt_layer(target));

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    CHECK(stroke.target_layer() == target);
    stroke.stamp(MeshBrush::Draw, dab(cf3(-0.4f, 0.0f, 0.0f)));
    // The host switches channel mid-gesture. Set-active is metadata and moves
    // no vertex, so it is deliberately ALLOWED while the composition is held —
    // and it must not move where the rest of the stamps land. The transaction
    // re-pins its target per dab for exactly this; pinning once at `begin` was
    // the bug this case was written against, and it split the gesture in two.
    REQUIRE(surface.set_active_sculpt_layer(other));
    stroke.stamp(MeshBrush::Draw, dab(cf3(0.4f, 0.0f, 0.0f)));

    SculptLayerDelta record;
    REQUIRE(stroke.commit(&record));
    CHECK(record.layer() == target);
    CHECK(layer_energy(surface, target, 2) > 0.0);
    CHECK(layer_energy(surface, other, 2) == doctest::Approx(0.0));
    // And the stack's active layer is put back where the GESTURE found it: for
    // its duration the active layer belongs to the transaction, so a change
    // made inside it is taken back with the rest of the gesture's bookkeeping.
    CHECK(surface.sculpt_layers().active() == target);
}

TEST_CASE("a mirrored gesture is one layer, one record and the union of both sides") {
    // REQUIREMENT 3.5, and it is a property of the transaction rather than of a
    // symmetry feature: a mirrored write is another stamp inside the same
    // begin/commit, so it enters the pinned channel and joins the one record
    // whose coverage is both sides. Nothing here needs a mirror mode to exist —
    // if one is added, this is the contract it has to keep.
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("brow");
    REQUIRE(surface.set_active_sculpt_layer(id));

    const std::uint32_t left = nearest(surface, 2, cf3(-0.5f, 0.0f, 0.0f));
    const std::uint32_t right = nearest(surface, 2, cf3(0.5f, 0.0f, 0.0f));
    REQUIRE(left != right);

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    for (int i = 0; i < 3; ++i) {
        const float z = -0.1f + 0.1f * static_cast<float>(i);
        const MeshBrushSettings s = dab(cf3(-0.5f, 0.0f, z), 0.25f, 0.5f);
        stroke.stamp(MeshBrush::Draw, s);
        // The mirror image, exactly as a host emits it.
        MeshBrushSettings mirrored = s;
        mirrored.center.x = -mirrored.center.x;
        stroke.stamp(MeshBrush::Draw, mirrored);
    }
    SculptLayerDelta record;
    REQUIRE(stroke.commit(&record));

    CHECK(record.layer() == id);
    const std::vector<std::uint32_t> levels = record.levels();
    CHECK(levels.size() == 1);
    // BOTH SIDES in one record: the coverage is the union, so undoing it takes
    // back the left and the right together rather than leaving half a face.
    CHECK_FALSE(surface.sculpt_layer_detail(id, 2, left).zero());
    CHECK_FALSE(surface.sculpt_layer_detail(id, 2, right).zero());
    REQUIRE(surface.apply_sculpt_layer_delta(record, false));
    CHECK(surface.sculpt_layer_detail(id, 2, left).zero());
    CHECK(surface.sculpt_layer_detail(id, 2, right).zero());
}

TEST_CASE("the write domain is the caller's choice, resolved once") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("pass");

    SUBCASE("detail with no active layer refuses to begin rather than writing the form") {
        REQUIRE(surface.set_active_sculpt_layer(mesh::kNoSculptLayer));
        LayeredMultiresSculptor stroke(surface);
        stroke.set_write_domain(MultiresWriteDomain::Detail);
        CHECK_FALSE(stroke.begin());
        CHECK_FALSE(surface.sculpt_layer_composition_held());
    }
    SUBCASE("geometry writes the base even with a layer active") {
        REQUIRE(surface.set_active_sculpt_layer(id));
        const std::uint64_t layer_before = surface.sculpt_layer_checksum();
        LayeredMultiresSculptor stroke(surface);
        stroke.set_write_domain(MultiresWriteDomain::Geometry);
        REQUIRE(stroke.begin());
        CHECK(stroke.target_layer() == mesh::kNoSculptLayer);
        CHECK(stroke.stamp(MeshBrush::Draw, dab(cf3(0, 0, 0), 0.4f, 0.5f)) > 0);
        mesh::MultiresDelta base_record;
        REQUIRE(stroke.commit(nullptr, &base_record));
        CHECK_FALSE(base_record.empty());
        // The form under the passes moved and the pass did not.
        CHECK(surface.sculpt_layer_checksum() == layer_before);
    }
    SUBCASE("a base-domain gesture cancels exactly too") {
        REQUIRE(surface.set_active_sculpt_layer(id));
        const std::uint64_t base_before = surface.detail_checksum();
        LayeredMultiresSculptor stroke(surface);
        stroke.set_write_domain(MultiresWriteDomain::Geometry);
        REQUIRE(stroke.begin());
        stroke.stamp(MeshBrush::Draw, dab(cf3(0, 0, 0), 0.4f, 0.5f));
        REQUIRE(surface.detail_checksum() != base_before);
        stroke.cancel();
        CHECK(surface.detail_checksum() == base_before);
    }
    SUBCASE("automatic follows the active layer, and the base when there is none") {
        REQUIRE(surface.set_active_sculpt_layer(id));
        LayeredMultiresSculptor into_layer(surface);
        REQUIRE(into_layer.begin());
        CHECK(into_layer.target_layer() == id);
        into_layer.cancel();

        REQUIRE(surface.set_active_sculpt_layer(mesh::kNoSculptLayer));
        LayeredMultiresSculptor into_base(surface);
        REQUIRE(into_base.begin());
        CHECK(into_base.target_layer() == mesh::kNoSculptLayer);
        into_base.cancel();
    }
}

TEST_CASE("begin refuses what it cannot honour, and changes nothing when it does") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("finished");
    REQUIRE(surface.set_active_sculpt_layer(id));

    SUBCASE("a locked target") {
        REQUIRE(surface.set_sculpt_layer_locked(id, true));
        LayeredMultiresSculptor stroke(surface);
        CHECK_FALSE(stroke.begin());
        CHECK_FALSE(stroke.open());
        CHECK_FALSE(surface.sculpt_layer_composition_held());
    }
    SUBCASE("a stroke that is already open") {
        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK_FALSE(stroke.begin());
        stroke.cancel();
    }
    SUBCASE("and a verb outside a transaction does nothing at all") {
        const std::uint64_t layer_quiet = surface.sculpt_layer_checksum();
        const std::uint64_t base_quiet = surface.detail_checksum();
        LayeredMultiresSculptor stroke(surface);
        CHECK(stroke.stamp(MeshBrush::Draw, dab(cf3(0, 0, 0))) == 0);
        CHECK(stroke.erase(dab(cf3(0, 0, 0))) == 0);
        CHECK(stroke.restore(dab(cf3(0, 0, 0))) == 0);
        CHECK(stroke.smooth(MultiresSmoothMode::DetailOnly, dab(cf3(0, 0, 0))) == 0);
        CHECK(surface.sculpt_layer_checksum() == layer_quiet);
        CHECK(surface.detail_checksum() == base_quiet);
    }
}

TEST_CASE("an open stroke holds the composition and lets the metadata through") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("pass");
    const SculptLayerId under = surface.add_sculpt_layer("under");
    REQUIRE(surface.move_sculpt_layer(under, 0));
    REQUIRE(surface.set_active_sculpt_layer(id));

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    CHECK(surface.sculpt_layer_composition_held());

    // Refused rather than deferred, because a slider that appears to move and
    // then silently applies later is the worse surprise.
    CHECK_FALSE(surface.set_sculpt_layer_strength(under, 0.5f));
    CHECK_FALSE(surface.set_sculpt_layer_visible(under, false));
    CHECK_FALSE(surface.remove_sculpt_layer(under));
    CHECK_FALSE(surface.move_sculpt_layer(under, 1));
    CHECK_FALSE(surface.merge_sculpt_layer_down(id));
    CHECK(surface.add_sculpt_layer("while open") == mesh::kNoSculptLayer);
    CHECK(surface.sculpt_layers().find(under)->strength == 1.0f);
    CHECK(surface.sculpt_layers().size() == 2);

    // None of these moves a vertex, so none of them is held.
    CHECK(surface.rename_sculpt_layer(under, "renamed"));
    CHECK(surface.set_sculpt_layer_locked(under, true));
    CHECK(surface.set_active_sculpt_layer(under));

    REQUIRE(stroke.commit());
    CHECK_FALSE(surface.sculpt_layer_composition_held());
    CHECK(surface.set_sculpt_layer_strength(under, 0.5f));
}

TEST_CASE("erase moves the active channel toward zero and reaches nothing else") {
    MultiresSurface surface = build(2);
    surface.set_detail(2, 40, LocalDetail{0.0f, 0.0f, 0.05f});
    const SculptLayerId lower = surface.add_sculpt_layer("lower");
    const SculptLayerId upper = surface.add_sculpt_layer("upper");
    for (std::uint32_t v = 30; v < 60; ++v) {
        surface.set_sculpt_layer_detail(lower, 2, v, LocalDetail{0.0f, 0.0f, 0.02f});
        surface.set_sculpt_layer_detail(upper, 2, v, LocalDetail{0.0f, 0.0f, 0.03f});
    }
    REQUIRE(surface.set_active_sculpt_layer(upper));
    const std::uint64_t base_before = surface.detail_checksum();
    const double lower_before = layer_energy(surface, lower, 2);
    const double upper_before = layer_energy(surface, upper, 2);

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    CHECK(stroke.erase(dab(surface.positions_at(2)[40], 0.5f, 1.0f)) > 0);
    REQUIRE(stroke.commit());

    CHECK(layer_energy(surface, upper, 2) < upper_before);
    // NEITHER the base nor the layer beneath: an eraser for THIS pass, not a
    // flattening brush.
    CHECK(surface.detail_checksum() == base_before);
    CHECK(layer_energy(surface, lower, 2) == doctest::Approx(lower_before));

    SUBCASE("and with no target there is nothing to erase") {
        REQUIRE(surface.set_active_sculpt_layer(mesh::kNoSculptLayer));
        LayeredMultiresSculptor base_stroke(surface);
        REQUIRE(base_stroke.begin());
        CHECK(base_stroke.erase(dab(cf3(0, 0, 0), 0.5f, 1.0f)) == 0);
        CHECK(surface.detail_checksum() == base_before);
        base_stroke.cancel();
    }
}

TEST_CASE("restore fades the level's own detail and leaves every layer standing") {
    MultiresSurface surface = build(2);
    for (std::uint32_t v = 30; v < 60; ++v)
        surface.set_detail(2, v, LocalDetail{0.0f, 0.0f, 0.04f});
    const SculptLayerId id = surface.add_sculpt_layer("pores");
    for (std::uint32_t v = 30; v < 60; ++v)
        surface.set_sculpt_layer_detail(id, 2, v, LocalDetail{0.0f, 0.0f, 0.01f});
    REQUIRE(surface.set_active_sculpt_layer(id));
    const std::uint64_t base_before = surface.detail_checksum();
    const std::uint64_t layer_before = surface.sculpt_layer_checksum();

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    CHECK(stroke.restore(dab(surface.positions_at(2)[40], 0.5f, 1.0f)) > 0);
    REQUIRE(stroke.commit());
    CHECK(surface.detail_checksum() != base_before);
    // The form went back toward the pure subdivision and the pass rode through.
    CHECK(surface.sculpt_layer_checksum() == layer_before);

    SUBCASE("and level 0 has no pure subdivision to return to") {
        REQUIRE(surface.set_sculpt_level(0));
        LayeredMultiresSculptor cage(surface);
        REQUIRE(cage.begin());
        CHECK(cage.restore(dab(cf3(0, 0, 0), 0.5f, 1.0f)) == 0);
        cage.cancel();
    }
}

TEST_CASE("the three smoothing modes act on three different arrays") {
    // A plain Laplacian over pores removes the pores, which is rarely what was
    // asked — so each mode is asserted by what it left alone.
    const auto fixture = [](MultiresSurface* surface, SculptLayerId* id) {
        *surface = build(2);
        for (std::uint32_t v = 20; v < 80; ++v)
            surface->set_detail(2, v, LocalDetail{0.0f, 0.0f, (v % 2) ? 0.03f : -0.03f});
        *id = surface->add_sculpt_layer("pores");
        for (std::uint32_t v = 20; v < 80; ++v)
            surface->set_sculpt_layer_detail(*id, 2, v,
                                             LocalDetail{0.0f, 0.0f, (v % 3) ? 0.01f : -0.01f});
        REQUIRE(surface->set_active_sculpt_layer(*id));
    };

    SUBCASE("detail_only touches the target channel and nothing under it") {
        MultiresSurface surface;
        SculptLayerId id = 0;
        fixture(&surface, &id);
        const std::uint64_t base_before = surface.detail_checksum();
        const double energy_before = layer_energy(surface, id, 2);

        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.smooth(MultiresSmoothMode::DetailOnly,
                            dab(surface.positions_at(2)[40], 0.6f, 1.0f)) > 0);
        REQUIRE(stroke.commit());
        // An alternating field averaged toward its neighbours loses magnitude.
        CHECK(layer_energy(surface, id, 2) < energy_before);
        CHECK(surface.detail_checksum() == base_before);
    }
    SUBCASE("preserve_detail smooths the form and carries every layer through") {
        MultiresSurface surface;
        SculptLayerId id = 0;
        fixture(&surface, &id);
        const std::uint64_t layer_before = surface.sculpt_layer_checksum();
        const std::uint64_t base_before = surface.detail_checksum();

        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.smooth(MultiresSmoothMode::PreserveDetail,
                            dab(surface.positions_at(2)[40], 0.6f, 1.0f)) > 0);
        REQUIRE(stroke.commit());
        CHECK(surface.detail_checksum() != base_before);
        // BIT FOR BIT: the pass rode through the correction untouched, which is
        // the whole reason this mode exists.
        CHECK(surface.sculpt_layer_checksum() == layer_before);
    }
    SUBCASE("geometry is exactly the Smooth brush, so it lands in the channel") {
        MultiresSurface surface;
        SculptLayerId id = 0;
        fixture(&surface, &id);
        const std::uint64_t base_before = surface.detail_checksum();

        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.smooth(MultiresSmoothMode::Geometry,
                            dab(surface.positions_at(2)[40], 0.6f, 1.0f)) > 0);
        REQUIRE(stroke.commit());
        // The active layer is where a verb lands, and Geometry IS a verb.
        CHECK(surface.detail_checksum() == base_before);
    }
}

TEST_CASE("a height stamp deposits along the normal and a vector stamp in the frame") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("map");
    REQUIRE(surface.set_active_sculpt_layer(id));
    const std::uint32_t centre_vertex = nearest(surface, 2, cf3(0, 0, 0));

    // A CONSTANT map, so what the case measures is where the value landed
    // rather than what the bilinear read returned.
    std::vector<float> height(16 * 16, 1.0f);
    std::vector<float> vector_map(3 * 16 * 16, 0.0f);
    for (std::size_t i = 0; i < 16 * 16; ++i) vector_map[i] = 1.0f;  // tangent plane only

    SUBCASE("height moves the third coefficient and only the third") {
        DetailStampSettings stamp;
        stamp.mode = DetailStampMode::Height;
        stamp.image = height.data();
        stamp.width = 16;
        stamp.height = 16;
        stamp.amplitude = 0.1f;
        stamp.center = surface.positions_at(2)[centre_vertex];
        stamp.extent = 1.0f;

        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.stamp_detail(stamp, dab(stamp.center, 0.5f, 1.0f)) > 0);
        REQUIRE(stroke.commit());

        const LocalDetail d = surface.sculpt_layer_detail(id, 2, centre_vertex);
        CHECK(d.normal > 0.0f);
        CHECK(d.tangent == 0.0f);
        CHECK(d.bitangent == 0.0f);
    }
    SUBCASE("a vector map's first plane is the TANGENT, never a world axis") {
        DetailStampSettings stamp;
        stamp.mode = DetailStampMode::Vector;
        stamp.image = vector_map.data();
        stamp.width = 16;
        stamp.height = 16;
        stamp.amplitude = 0.1f;
        stamp.center = surface.positions_at(2)[centre_vertex];
        stamp.extent = 1.0f;

        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.stamp_detail(stamp, dab(stamp.center, 0.5f, 1.0f)) > 0);
        REQUIRE(stroke.commit());

        // The three planes are tangent, bitangent and normal OF THE VERTEX'S
        // FRAME. A world-space reading would have put this into whichever axis
        // the plane happens to lie along and made the same map shear across a
        // curve.
        const LocalDetail d = surface.sculpt_layer_detail(id, 2, centre_vertex);
        CHECK(d.tangent > 0.0f);
        CHECK(d.bitangent == 0.0f);
        CHECK(d.normal == 0.0f);
    }
    SUBCASE("a scalar weight is refused rather than served twice") {
        DetailStampSettings stamp;
        stamp.mode = DetailStampMode::Weight;
        stamp.image = height.data();
        stamp.width = 16;
        stamp.height = 16;
        stamp.center = cf3(0, 0, 0);
        stamp.extent = 1.0f;

        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.stamp_detail(stamp, dab(cf3(0, 0, 0), 0.5f, 1.0f)) == 0);
        CHECK(stroke.record_size() == 0);
        stroke.cancel();
    }
    SUBCASE("an invalid map is refused before anything is gathered") {
        DetailStampSettings stamp;
        stamp.mode = DetailStampMode::Height;
        stamp.image = nullptr;
        stamp.width = 16;
        stamp.height = 16;
        CHECK_FALSE(stamp.valid());
        LayeredMultiresSculptor stroke(surface);
        REQUIRE(stroke.begin());
        CHECK(stroke.stamp_detail(stamp, dab(cf3(0, 0, 0), 0.5f, 1.0f)) == 0);
        CHECK(stroke.stamps() == 0);
        stroke.cancel();
    }
}

TEST_CASE("the stamp reports the resolution it does not have rather than blurring it") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("map");
    REQUIRE(surface.set_active_sculpt_layer(id));

    std::vector<float> fine(512 * 512, 1.0f);
    DetailStampSettings stamp;
    stamp.mode = DetailStampMode::Height;
    stamp.image = fine.data();
    stamp.width = 512;
    stamp.height = 512;
    stamp.amplitude = 0.02f;
    stamp.center = surface.positions_at(2)[nearest(surface, 2, cf3(0, 0, 0))];
    stamp.extent = 0.4f;

    LayeredMultiresSculptor stroke(surface);
    REQUIRE(stroke.begin());
    CHECK(stroke.stamp_detail(stamp, dab(stamp.center, 0.4f, 1.0f)) > 0);
    REQUIRE(stroke.commit());

    const mesh::DetailStampReport& report = stroke.last_stamp_report();
    CHECK(report.vertex_spacing > 0.0f);
    CHECK(report.sample_size == doctest::Approx(0.4f / 512.0f));
    // 512 samples across 0.4 world units, over a level whose edges are far
    // wider: the map carries what the level cannot hold, and the library that
    // implied the resolution says it does not have it.
    CHECK(report.oversampling > 1.0f);
    CHECK(report.under_resolved);

    SUBCASE("and a map the level can carry is not flagged") {
        std::vector<float> coarse(4 * 4, 1.0f);
        DetailStampSettings easy = stamp;
        easy.image = coarse.data();
        easy.width = 4;
        easy.height = 4;
        easy.extent = 1.5f;
        LayeredMultiresSculptor second(surface);
        REQUIRE(second.begin());
        second.stamp_detail(easy, dab(easy.center, 0.5f, 1.0f));
        CHECK_FALSE(second.last_stamp_report().under_resolved);
        second.cancel();
    }
}

TEST_CASE("outside the stamp square is nothing, not the clamped border row") {
    // A clamped border is right for a scalar alpha, whose radial weight ends
    // the influence anyway, and wrong for a displacement — it would smear the
    // map's edge row across everything the brush reaches beyond the square.
    std::vector<float> image(8 * 8, 1.0f);
    DetailStampSettings stamp;
    stamp.mode = DetailStampMode::Height;
    stamp.image = image.data();
    stamp.width = 8;
    stamp.height = 8;
    stamp.amplitude = 1.0f;
    stamp.center = cf3(0, 0, 0);
    stamp.direction = cf3(0, 1, 0);
    stamp.extent = 1.0f;

    const mesh::AlphaFrame frame = mesh::detail_stamp_frame(stamp, cf3(0, 1, 0));
    mesh::SurfaceFrame vertex;
    vertex.normal = cf3(0, 1, 0);
    vertex.tangent = cf3(1, 0, 0);
    vertex.bitangent = cf3(0, 0, 1);

    CHECK(mesh::detail_stamp_sample(stamp, frame, vertex, cf3(0, 0, 0)).inside);
    const mesh::DetailStampSample outside =
        mesh::detail_stamp_sample(stamp, frame, vertex, cf3(4.0f, 0.0f, 0.0f));
    CHECK_FALSE(outside.inside);
    CHECK(outside.offset.zero());
}
