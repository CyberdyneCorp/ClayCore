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
        // Task 5.3's three revisions, each read BEFORE the rename. Comparing
        // the composition revision to a second read of itself — which is what
        // this case did until the test suite was audited — is a check that
        // cannot fail, and the whole claim of 5.2 lives in it: a name is
        // metadata, and metadata must not invalidate geometry.
        const std::uint64_t metadata = surface.sculpt_layer_metadata_revision();
        const std::uint64_t composition = surface.sculpt_layer_composition_revision();
        const std::uint64_t content = surface.sculpt_layer_content_revision();
        REQUIRE(surface.rename_sculpt_layer(id, "pores"));
        surface.positions_at(2);
        CHECK(surface.sculpt_layer_stats().blocks_recomposed == 0);
        CHECK(surface.sculpt_layer_metadata_revision() > metadata);
        CHECK(surface.sculpt_layer_composition_revision() == composition);
        CHECK(surface.sculpt_layer_content_revision() == content);
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
    //
    // This is the CHEAP half of the claim and it is worth saying which half:
    // `move_to` invalidates no block, so what this asserts is that a reorder
    // does not disturb a composed cache that is already there. The half that
    // can actually catch an order-dependent composition is the case below,
    // which recomposes after the reorder.
    CHECK(bit_equal(before, a.positions_at(2)));
}

TEST_CASE("a reorder still moves no vertex once the blocks it holds recompose") {
    // THE SAME CLAIM, ASSERTED WHERE IT CAN ACTUALLY FAIL — and it did. The
    // case above cannot see the interesting half of task 3.1 for two
    // independent reasons, which is how the defect this pins survived:
    //
    //   * `move_to` invalidates NO block, on purpose, so reading the surface
    //     straight after a reorder reads the composed cache back unchanged.
    //     That comparison is the cache against itself whatever composition
    //     does;
    //   * two layers over a zero base sum as `(0 + x) + y` against `(0 + y) +
    //     x`, and a single IEEE addition commutes exactly. Order can only bite
    //     from the third term on, or from the second over a non-zero base,
    //     because float addition does not ASSOCIATE.
    //
    // So: three deep, on a base detail that is already there, with the blocks
    // driven through composition again AFTER the reorder. The numbers are
    // chosen rather than pretty — 0.4·0.03 twice and 0.7·0.03 over a base of
    // 0.05 is a sum whose value depends on the order it is taken in, at
    // 0.0949999988 against 0.0950000063.
    MultiresSurface surface = build(2);
    for (std::uint32_t v = 30; v < 50; ++v) surface.set_detail(2, v, lift(0.05f));
    const SculptLayerId a = surface.add_sculpt_layer("a");
    const SculptLayerId b = surface.add_sculpt_layer("b");
    const SculptLayerId c = surface.add_sculpt_layer("c");
    for (std::uint32_t v = 30; v < 50; ++v) {
        surface.set_sculpt_layer_detail(a, 2, v, lift(0.03f));
        surface.set_sculpt_layer_detail(b, 2, v, lift(0.03f));
        surface.set_sculpt_layer_detail(c, 2, v, lift(0.03f));
    }
    REQUIRE(surface.set_sculpt_layer_strength(a, 0.4f));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.4f));
    REQUIRE(surface.set_sculpt_layer_strength(c, 0.7f));
    const std::vector<cfloat3> before = surface.positions_at(2);

    REQUIRE(surface.move_sculpt_layer(c, 0));
    // A slider away and back is the cheapest way to make every block this
    // layer covers compose a second time. It ends on the value it started
    // from, so the only thing that changed between the two readings is the
    // order the stack is listed in.
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.55f));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.4f));
    CHECK(bit_equal(before, surface.positions_at(2)));

    // And the other direction, so the case is not passing because the reorder
    // happened to be undone: put the stack back and recompose again.
    REQUIRE(surface.move_sculpt_layer(c, 2));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.55f));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.4f));
    CHECK(bit_equal(before, surface.positions_at(2)));
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

TEST_CASE("merge folds the per-layer mask in, once, and leaves the identity behind") {
    // THE HALF OF THE PARITY GATE THE MASK WAS MISSING FROM. Task 2.7's
    // per-layer mask is a SECOND multiplier in `E = B + Σ sᵢ·mᵢ(v)·Lᵢ`, and a
    // merge that is defined by the surface it leaves has to fold it into the
    // coefficients it writes AND clear it — a mask left standing would apply
    // itself a second time to a coefficient that already carries it, and one
    // dropped would lose the shape the artist masked. Every merge and bake case
    // in this suite ran with the identity mask, so the arithmetic that handles a
    // real one was written and never asked a question.
    for (float lower_strength : {1.0f, 0.37f, 0.0f}) {
        CAPTURE(lower_strength);
        MultiresSurface surface = build(2);
        const SculptLayerId lower = surface.add_sculpt_layer("lower");
        const SculptLayerId upper = surface.add_sculpt_layer("upper");
        for (std::uint32_t v = 30; v < 50; ++v) {
            surface.set_sculpt_layer_detail(lower, 2, v, lift(0.04f));
            surface.set_sculpt_layer_detail(upper, 2, v, lift(0.02f));
            // Two masks that DISAGREE and that vary along the run, so a merge
            // dropping either one, or applying one of them twice, lands
            // somewhere a constant mask would have hidden.
            const float t = static_cast<float>(v - 30);
            REQUIRE(surface.set_sculpt_layer_mask(lower, 2, v, 0.25f + 0.03f * t));
            REQUIRE(surface.set_sculpt_layer_mask(upper, 2, v, 0.95f - 0.04f * t));
        }
        REQUIRE(surface.set_sculpt_layer_strength(lower, lower_strength));
        REQUIRE(surface.set_sculpt_layer_strength(upper, 0.6f));
        const std::vector<cfloat3> before = surface.positions_at(2);

        REQUIRE(surface.merge_sculpt_layer_down(upper));
        REQUIRE(surface.sculpt_layers().size() == 1);
        const std::vector<cfloat3> after = surface.positions_at(2);
        for (std::size_t i = 0; i < before.size(); ++i) {
            CAPTURE(i);
            CHECK(after[i].y == doctest::Approx(before[i].y).epsilon(1e-6));
        }
        // The identity the target needs is the mask's as well as the slider's:
        // the weight is in the coefficients now, so the mask has to be gone
        // rather than combined.
        const mesh::SparseWeightField* mask = surface.sculpt_layers().mask_at(lower, 2);
        REQUIRE(mask != nullptr);
        for (std::uint32_t v = 30; v < 50; ++v) CHECK(mask->get(v) == 1.0f);
    }
}

TEST_CASE("bake carries the mask into the base, and the surface stays where it was") {
    MultiresSurface surface = build(2);
    for (std::uint32_t v = 30; v < 50; ++v) surface.set_detail(2, v, lift(0.011f));
    const SculptLayerId id = surface.add_sculpt_layer("pass");
    for (std::uint32_t v = 30; v < 50; ++v) {
        surface.set_sculpt_layer_detail(id, 2, v, lift(0.03f));
        REQUIRE(surface.set_sculpt_layer_mask(id, 2, v, 0.2f + 0.04f * static_cast<float>(v - 30)));
    }
    REQUIRE(surface.set_sculpt_layer_strength(id, 0.55f));
    const std::vector<cfloat3> before = surface.positions_at(2);

    REQUIRE(surface.bake_sculpt_layer_to_base(id));
    CHECK(surface.sculpt_layers().empty());
    const std::vector<cfloat3> after = surface.positions_at(2);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CAPTURE(i);
        CHECK(after[i].y == doctest::Approx(before[i].y).epsilon(1e-6));
    }
    // The base has neither a strength nor a mask to carry, so what landed there
    // must be the MASKED, SCALED coefficient and not the raw one — a bake that
    // ignored the mask would write 0.011 + 0.55·0.03 at every masked vertex.
    const float w = 0.2f + 0.04f * 10.0f;
    CHECK(surface.detail_at(2).get(40).normal ==
          doctest::Approx(0.011f + 0.55f * w * 0.03f).epsilon(1e-5));
    CHECK(surface.memory().composed == 0);
}

TEST_CASE("compacting is a memory lever, not a change to the picture") {
    // Task 5.7 leaves a host four levers instead of a cap, and `compact` is the
    // one that costs nothing to reach for. The case beside it asserts that the
    // bytes go down — which is also what a lever that ATE THE PASS would do.
    // What has to hold is that the surface does not move: not immediately,
    // where the composed cache would answer for it whatever compaction did, and
    // not once every covered block has composed again out of what survived.
    MultiresSurface surface = build(4);
    const std::uint32_t level = 4;
    const std::uint32_t vertices = surface.topology_at(level).vertex_count;
    const SculptLayerId a = surface.add_sculpt_layer("a");
    const SculptLayerId b = surface.add_sculpt_layer("b");
    for (std::uint32_t v = 0; v < vertices; ++v) {
        if (v % 3 == 0) surface.set_sculpt_layer_detail(a, level, v, lift(0.004f));
        if (v >= 1000 && v < 1400) surface.set_sculpt_layer_detail(b, level, v, lift(-0.002f));
        // A mask that is real over part of the layer and identity over the
        // rest, so compaction has identity blocks to drop AND weighted ones it
        // must not.
        if (v % 7 == 0) REQUIRE(surface.set_sculpt_layer_mask(a, level, v, 0.6f));
    }
    REQUIRE(surface.set_sculpt_layer_strength(a, 0.8f));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.45f));
    // A whole block written back to zero, which is what actually gives
    // compaction something to release.
    for (std::uint32_t v = 1024; v < 2048 && v < vertices; ++v)
        surface.set_sculpt_layer_detail(a, level, v, lift(0.0f));
    const std::vector<cfloat3> before = surface.positions_at(level);
    const std::size_t bytes_before = surface.memory().sculpt_layers;

    {
        const mesh::SculptLayer* la = surface.sculpt_layers().find(a);
        const mesh::SculptLayer* lb = surface.sculpt_layers().find(b);
        MESSAGE("PRE a.detail dense=" << la->detail[level].dense()
                << " blocks=" << la->detail[level].stored_block_count()
                << " bytes=" << la->detail[level].bytes()
                << " | a.mask blocks=" << la->mask[level].stored_block_count()
                << " bytes=" << la->mask[level].bytes()
                << " | b.detail dense=" << lb->detail[level].dense()
                << " blocks=" << lb->detail[level].stored_block_count()
                << " bytes=" << lb->detail[level].bytes());
    }
    surface.compact_sculpt_layers();
    {
        const mesh::SculptLayer* la = surface.sculpt_layers().find(a);
        const mesh::SculptLayer* lb = surface.sculpt_layers().find(b);
        MESSAGE("POST a.detail dense=" << la->detail[level].dense()
                << " blocks=" << la->detail[level].stored_block_count()
                << " bytes=" << la->detail[level].bytes()
                << " | a.mask blocks=" << la->mask[level].stored_block_count()
                << " bytes=" << la->mask[level].bytes()
                << " | b.detail dense=" << lb->detail[level].dense()
                << " blocks=" << lb->detail[level].stored_block_count()
                << " bytes=" << lb->detail[level].bytes());
    }
    MESSAGE("bytes_before=" << bytes_before
            << " after=" << surface.memory().sculpt_layers
            << " vertices=" << vertices);
    CHECK(surface.memory().sculpt_layers < bytes_before);
    CHECK(bit_equal(before, surface.positions_at(level)));

    // And the reading that matters: dial both sliders away and back so every
    // block either layer covers is composed again from the storage compaction
    // left behind.
    REQUIRE(surface.set_sculpt_layer_strength(a, 0.5f));
    REQUIRE(surface.set_sculpt_layer_strength(a, 0.8f));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.5f));
    REQUIRE(surface.set_sculpt_layer_strength(b, 0.45f));
    CHECK(bit_equal(before, surface.positions_at(level)));
}
