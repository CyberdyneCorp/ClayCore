// THE STACK'S SEMANTICS (mesh-sculpt-layers spec, add-mesh-sculpt-layers).
//
// Every case here is a claim from the change's gate table, and the ones that
// compare BITS rather than tolerances do so deliberately:
//
//   * a hierarchy with no layers must evaluate to the SAME BITS it did before
//     layers existed. An implementation that always composed — even summing an
//     empty stack — would move the last bit of every vertex and break every
//     existing multires golden for nothing;
//   * strength 0 and invisible must contribute NOTHING, which is the same
//     statement one step on: a layer at zero is skipped rather than multiplied
//     by zero, so the composed field is the base field bit for bit;
//   * additive layers COMMUTE, so swapping two overlapping layers must leave
//     the evaluated surface unchanged to the last bit. A tolerance here would
//     pass an implementation that accumulates in stack order and rounds.
//
// The one that is arithmetic rather than a bit compare is the strength dial: a
// layer at 0.5 must move the surface by half of what it stores, and raising it
// to 1 must double that — because a stroke records its FULL contribution and
// nothing in this change divides by a strength.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt_layer.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MultiresSurface;
using mesh::SculptLayerId;
using mesh::SculptLayerStack;

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
            const std::uint32_t a = static_cast<std::uint32_t>(z) * stride +
                                    static_cast<std::uint32_t>(x);
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
    return std::move(*surface);
}

bool bit_equal(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

LocalDetail lift(float n) {
    LocalDetail d;
    d.normal = n;
    return d;
}

}  // namespace

TEST_CASE("an empty stack evaluates to exactly the bits it did before layers existed") {
    MultiresSurface surface = build(2);
    surface.set_detail(2, 40, lift(0.05f));
    const std::vector<cfloat3> before = surface.positions_at(2);

    const SculptLayerId id = surface.add_sculpt_layer("wrinkles");
    CHECK(id != mesh::kNoSculptLayer);
    // A layer with nothing stored reaches no level, so no level allocates a
    // composed field and `apply_detail` reads the base through the same call.
    CHECK(bit_equal(before, surface.positions_at(2)));
    CHECK(surface.memory().composed == 0);
}

TEST_CASE("strength dials a recorded pass without replaying it") {
    MultiresSurface surface = build(2);
    surface.set_detail(2, 40, lift(0.05f));
    const std::vector<cfloat3> base = surface.positions_at(2);

    // The plane's own normal decides which way a positive coefficient moves
    // the vertex, so the expected offset is read from the frame rather than
    // assumed — a coefficient is measured against the surface, which is the
    // whole point of storing one.
    const float ny = surface.frames_at(2)[40].normal.y;
    const SculptLayerId id = surface.add_sculpt_layer("wrinkles");
    surface.set_sculpt_layer_detail(id, 2, 40, lift(0.2f));
    const cfloat3 full = surface.positions_at(2)[40];
    const float lifted = full.y - base[40].y;
    CHECK(lifted == doctest::Approx(0.2f * ny));

    SUBCASE("zero contributes nothing, to the bit") {
        REQUIRE(surface.set_sculpt_layer_strength(id, 0.0f));
        CHECK(bit_equal(base, surface.positions_at(2)));
    }
    SUBCASE("invisible contributes nothing, to the bit") {
        REQUIRE(surface.set_sculpt_layer_visible(id, false));
        CHECK(bit_equal(base, surface.positions_at(2)));
    }
    SUBCASE("half contributes half, and no stroke is replayed") {
        surface.reset_eval_stats();
        REQUIRE(surface.set_sculpt_layer_strength(id, 0.5f));
        const cfloat3 half = surface.positions_at(2)[40];
        CHECK(half.y - base[40].y == doctest::Approx(0.1f * ny));
        // The coefficients are untouched: the pass was recorded at full size
        // and the dial is composition. Raising it back restores the whole
        // contribution rather than a scaled-down remnant.
        CHECK(surface.sculpt_layer_detail(id, 2, 40).normal == doctest::Approx(0.2f));
        REQUIRE(surface.set_sculpt_layer_strength(id, 1.0f));
        CHECK(surface.positions_at(2)[40].y == doctest::Approx(full.y));
    }
    SUBCASE("a rename invalidates no geometry") {
        surface.positions_at(2);
        surface.reset_sculpt_layer_stats();
        REQUIRE(surface.rename_sculpt_layer(id, "pores"));
        surface.positions_at(2);
        CHECK(surface.sculpt_layer_stats().blocks_recomposed == 0);
        CHECK(surface.sculpt_layer_composition_revision() ==
              surface.sculpt_layer_composition_revision());
    }
}

TEST_CASE("additive layers commute") {
    MultiresSurface a = build(2);
    const SculptLayerId low = a.add_sculpt_layer("low");
    const SculptLayerId high = a.add_sculpt_layer("high");
    for (std::uint32_t v = 30; v < 50; ++v) {
        a.set_sculpt_layer_detail(low, 2, v, lift(0.03f));
        a.set_sculpt_layer_detail(high, 2, v, lift(-0.01f));
    }
    a.set_sculpt_layer_strength(low, 0.4f);
    a.set_sculpt_layer_strength(high, 0.7f);
    const std::vector<cfloat3> before = a.positions_at(2);

    REQUIRE(a.move_sculpt_layer(high, 0));
    CHECK(a.sculpt_layers().id_at(0) == high);
    // Reordering changes ORGANISATION and not geometry, and the requirement
    // says so rather than implying an order dependence that does not exist.
    CHECK(bit_equal(before, a.positions_at(2)));
}

TEST_CASE("merge down is defined by the surface it leaves, including at zero strength") {
    for (float lower_strength : {1.0f, 0.37f, 0.0f}) {
        MultiresSurface surface = build(2);
        const SculptLayerId lower = surface.add_sculpt_layer("lower");
        const SculptLayerId upper = surface.add_sculpt_layer("upper");
        for (std::uint32_t v = 30; v < 50; ++v) {
            surface.set_sculpt_layer_detail(lower, 2, v, lift(0.04f));
            surface.set_sculpt_layer_detail(upper, 2, v, lift(0.02f));
        }
        REQUIRE(surface.set_sculpt_layer_strength(lower, lower_strength));
        REQUIRE(surface.set_sculpt_layer_strength(upper, 0.6f));
        const std::vector<cfloat3> before = surface.positions_at(2);

        // The naive concatenation divides by the lower layer's strength and is
        // undefined at exactly the value one slider reaches. This sets the
        // target to the identity it needs instead, so parity holds everywhere.
        REQUIRE(surface.merge_sculpt_layer_down(upper));
        CHECK(surface.sculpt_layers().size() == 1);
        const std::vector<cfloat3> after = surface.positions_at(2);
        for (std::size_t i = 0; i < before.size(); ++i)
            CHECK(after[i].y == doctest::Approx(before[i].y).epsilon(1e-6));
    }
}

TEST_CASE("bake to base leaves the surface where it was and the stack one shorter") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("pass");
    for (std::uint32_t v = 30; v < 50; ++v) surface.set_sculpt_layer_detail(id, 2, v, lift(0.03f));
    REQUIRE(surface.set_sculpt_layer_strength(id, 0.5f));
    const std::vector<cfloat3> before = surface.positions_at(2);

    REQUIRE(surface.bake_sculpt_layer_to_base(id));
    CHECK(surface.sculpt_layers().empty());
    const std::vector<cfloat3> after = surface.positions_at(2);
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK(after[i].y == doctest::Approx(before[i].y).epsilon(1e-6));
    // And with no layer left, the level reads its own base field again.
    CHECK(surface.memory().composed == 0);
}

TEST_CASE("removing a layer touches only its own coverage") {
    MultiresSurface surface = build(2);
    const SculptLayerId a = surface.add_sculpt_layer("a");
    const SculptLayerId b = surface.add_sculpt_layer("b");
    for (std::uint32_t v = 30; v < 50; ++v) {
        surface.set_sculpt_layer_detail(a, 2, v, lift(0.02f));
        surface.set_sculpt_layer_detail(b, 2, v, lift(0.01f));
    }
    const float ny = surface.frames_at(2)[40].normal.y;
    const std::vector<cfloat3> flat = build(2).positions_at(2);
    const float a_before = surface.sculpt_layer_detail(a, 2, 40).normal;

    REQUIRE(surface.remove_sculpt_layer(b));
    // No stroke is replayed and the other layer's coefficients do not move.
    CHECK(surface.sculpt_layer_detail(a, 2, 40).normal == a_before);
    CHECK(surface.positions_at(2)[40].y - flat[40].y == doctest::Approx(0.02f * ny));
}

TEST_CASE("a strength change costs the layer's coverage rather than the surface") {
    MultiresSurface surface = build(4);
    const std::uint32_t vertices = surface.topology_at(4).vertex_count;
    const std::uint32_t block = mesh::DetailField::kDefaultBlockSize;
    const std::uint32_t level_blocks = (vertices + block - 1) / block;
    REQUIRE(level_blocks >= 4);

    const SculptLayerId id = surface.add_sculpt_layer("local");
    // A footprint inside ONE block of a level made of several.
    for (std::uint32_t v = 10; v < 40; ++v) surface.set_sculpt_layer_detail(id, 4, v, lift(0.01f));
    surface.positions_at(4);

    surface.reset_sculpt_layer_stats();
    REQUIRE(surface.set_sculpt_layer_strength(id, 0.25f));
    surface.positions_at(4);
    // THE GATE: the blocks the layer has allocated, not the blocks the level
    // holds. Keyed on the layer's own storage rather than on a stack revision,
    // which is what makes this a data-structure property.
    CHECK(surface.sculpt_layer_stats().blocks_recomposed == 1);
    CHECK(surface.sculpt_layer_stats().blocks_recomposed < level_blocks);
}

TEST_CASE("a locked layer refuses a coefficient write and still accepts its properties") {
    MultiresSurface surface = build(2);
    const SculptLayerId id = surface.add_sculpt_layer("finished");
    surface.set_sculpt_layer_detail(id, 2, 40, lift(0.05f));
    REQUIRE(surface.set_sculpt_layer_locked(id, true));

    CHECK_FALSE(surface.set_sculpt_layer_detail(id, 2, 41, lift(0.05f)));
    CHECK(surface.sculpt_layer_detail(id, 2, 41).normal == 0.0f);
    // A lock is a permission on the coefficients and not on the channel.
    CHECK(surface.rename_sculpt_layer(id, "finished pass"));
    CHECK(surface.set_sculpt_layer_strength(id, 0.5f));
}

TEST_CASE("the stack round-trips inside the multires stream, and version 1 still loads") {
    MultiresSurface surface = build(2);
    const SculptLayerId first = surface.add_sculpt_layer("first");
    const SculptLayerId second = surface.add_sculpt_layer("second");
    surface.set_sculpt_layer_detail(first, 2, 40, lift(0.05f));
    surface.set_sculpt_layer_detail(second, 2, 41, lift(-0.02f));
    REQUIRE(surface.set_sculpt_layer_strength(first, 0.5f));
    REQUIRE(surface.set_sculpt_layer_mask(second, 2, 41, 0.25f));
    // A reorder BEFORE the save, so the test says something about ids rather
    // than about positions in a list.
    REQUIRE(surface.move_sculpt_layer(second, 0));

    const std::vector<std::uint8_t> bytes = surface.encode();
    MultiresSurface loaded;
    REQUIRE(MultiresSurface::decode(bytes.data(), bytes.size(), &loaded));
    REQUIRE(loaded.sculpt_layers().size() == 2);
    CHECK(loaded.sculpt_layers().id_at(0) == second);
    CHECK(loaded.sculpt_layers().id_at(1) == first);
    CHECK(loaded.sculpt_layers().find(first)->name == "first");
    CHECK(loaded.sculpt_layers().find(first)->strength == doctest::Approx(0.5f));
    CHECK(loaded.sculpt_layer_detail(first, 2, 40).normal == doctest::Approx(0.05f));
    CHECK(loaded.sculpt_layers().mask_at(second, 2)->get(41) == doctest::Approx(0.25f));
    CHECK(loaded.sculpt_layer_checksum() == surface.sculpt_layer_checksum());

    SUBCASE("a truncated stack chunk is refused rather than half loaded") {
        std::vector<std::uint8_t> cut = bytes;
        cut.resize(cut.size() - 16);
        MultiresSurface out;
        CHECK_FALSE(MultiresSurface::decode(cut.data(), cut.size(), &out));
    }
    SUBCASE("an absurd declared layer count is refused before allocation") {
        std::vector<std::uint8_t> hostile = SculptLayerStack{}.encode();
        // The layer count sits immediately after the magic and the version.
        hostile[8] = 0xff;
        hostile[9] = 0xff;
        hostile[10] = 0xff;
        hostile[11] = 0x0f;
        SculptLayerStack out;
        CHECK_FALSE(SculptLayerStack::decode(hostile.data(), hostile.size(), &out));
    }
}

TEST_CASE("a stroke into a layer records its full contribution and undoes exactly") {
    MultiresSurface surface = build(2);
    surface.set_detail(2, 40, lift(0.05f));
    const std::uint64_t base_checksum = surface.detail_checksum();

    const SculptLayerId id = surface.add_sculpt_layer("pass");
    REQUIRE(surface.set_sculpt_layer_strength(id, 0.5f));
    REQUIRE(surface.set_active_sculpt_layer(id));

    mesh::MultiresSculptor sculptor(surface);
    mesh::SculptLayerDelta record;
    mesh::MeshBrushSettings settings;
    settings.center = surface.positions_at(2)[40];
    settings.radius = 0.3f;
    settings.strength = 0.4f;
    surface.set_sculpt_level(2);
    const std::size_t moved = sculptor.stamp(mesh::MeshBrush::Draw, settings, {}, nullptr, &record);
    CHECK(moved > 0);
    CHECK(record.layer() == id);
    CHECK_FALSE(record.empty());
    // The BASE is untouched: with an active layer the gesture lands in the
    // channel and nowhere else.
    CHECK(surface.detail_checksum() == base_checksum);

    const std::vector<cfloat3> after = surface.positions_at(2);
    const std::uint64_t stroked_layer = surface.sculpt_layer_checksum();
    CHECK_FALSE(surface.sculpt_layers().find(id)->detail[2].empty());

    // Undo restores the layer EXACTLY — the recorded values, not a
    // recomputation — which a content checksum says and a position comparison
    // could not: two different coefficient sets can evaluate to the same
    // rounded position.
    REQUIRE(surface.apply_sculpt_layer_delta(record, false));
    CHECK(surface.sculpt_layers().find(id)->detail[2].empty());
    REQUIRE(surface.apply_sculpt_layer_delta(record, true));
    CHECK(surface.sculpt_layer_checksum() == stroked_layer);
    CHECK(bit_equal(after, surface.positions_at(2)));
}

TEST_CASE("a write on top of a deep stack sums only the layers that reach the block") {
    MultiresSurface surface = build(4);
    const std::uint32_t vertices = surface.topology_at(4).vertex_count;
    const std::uint32_t block = mesh::DetailField::kDefaultBlockSize;
    REQUIRE(vertices > 3 * block);

    // Sixteen layers, half over the first block and half over the fourth. The
    // two halves never meet, which is the whole point: a write into one must
    // not visit the other's layers.
    std::vector<SculptLayerId> near, far;
    for (int i = 0; i < 8; ++i) {
        const SculptLayerId a = surface.add_sculpt_layer("near");
        for (std::uint32_t v = 4; v < 300; ++v) surface.set_sculpt_layer_detail(a, 4, v, lift(0.001f));
        near.push_back(a);
        const SculptLayerId b = surface.add_sculpt_layer("far");
        for (std::uint32_t v = 3 * block + 4; v < 3 * block + 300; ++v)
            surface.set_sculpt_layer_detail(b, 4, v, lift(0.001f));
        far.push_back(b);
    }
    surface.positions_at(4);

    surface.reset_sculpt_layer_stats();
    surface.set_sculpt_layer_detail(near.back(), 4, 100, lift(0.01f));
    surface.positions_at(4);
    // THE GATE: one block recomposed, and inside it only the eight layers that
    // actually store coefficients there. The other eight are an O(1) miss on
    // their own block table and are never summed.
    CHECK(surface.sculpt_layer_stats().blocks_recomposed == 1);
    CHECK(surface.sculpt_layer_stats().layer_blocks_visited == 8);
}

// THE SHORT CIRCUIT MUST NOT SKIP AN OUTSTANDING RELEASE
// (add-extreme-poly-runtime; regression for a defect this change introduced).
//
// `evaluate_up_to` returns early when nothing below the target moved, which is
// what keeps a display-normal drain from costing the hierarchy. That short
// circuit asks `composition_pending`, and that function used to answer "no"
// the moment the stack was EMPTY -- which is exactly the state removing or
// baking the LAST layer leaves behind, with a composed field still allocated on
// every level the stack used to reach.
//
// The bytes are the visible half. The half that matters is that
// `composed_or_detail` PREFERS a composed field wherever one exists, so the
// level goes on reading the composed coefficients after its last layer is gone
// -- a baked layer contributing to the base it was just baked into.
//
// Asserted through a second evaluation with nothing pending, because the first
// one after a bake has patches marked and takes the full walk either way. It is
// the SECOND read that engages the short circuit, and it was green while the
// surface underneath it was wrong.
TEST_CASE("sculpt layers: removing the last layer releases the composed field") {
    MultiresSurface surface = build(2);
    const std::vector<cfloat3> pristine = surface.positions_at(2);

    const SculptLayerId id = surface.add_sculpt_layer("pass");
    for (std::uint32_t v = 30; v < 50; ++v) surface.set_sculpt_layer_detail(id, 2, v, lift(0.03f));
    const std::vector<cfloat3> with_layer = surface.positions_at(2);
    REQUIRE(surface.memory().composed > 0);
    // The fixture has to actually move something, or every check below passes
    // over a layer that never contributed.
    bool moved = false;
    for (std::size_t i = 0; i < pristine.size(); ++i)
        if (with_layer[i].y != pristine[i].y) moved = true;
    REQUIRE(moved);

    REQUIRE(surface.remove_sculpt_layer(id));
    surface.positions_at(2);              // the walk that has patches marked
    const std::vector<cfloat3> after = surface.positions_at(2);  // the short circuit

    CHECK(surface.memory().composed == 0);
    for (std::size_t i = 0; i < pristine.size(); ++i)
        CHECK(after[i].y == doctest::Approx(pristine[i].y).epsilon(1e-6));
}
