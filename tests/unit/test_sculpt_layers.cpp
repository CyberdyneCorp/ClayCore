// Sculpt layers on a voxel grid (voxel-engine spec, add-sculpt-layers).
//
// A pass you can dial back. The claims worth defending, in the order the
// proposal makes them: a layer at 0 is EXACTLY the grid without it, a layer at
// 1 is EXACTLY the pass applied directly, a fractional strength picks the same
// cells on every run, and the whole thing survives the file.
//
// "Exactly" throughout, not "close": the dither passes everything at 1 and
// nothing at 0 by construction, so anything less than identity is a defect.

#include <doctest/doctest.h>

#include <vector>

#include "clay/voxel/grid.h"

using namespace clay;
using namespace clay::kernel;
using voxel::BrushFalloff;
using voxel::BrushParams;
using voxel::BrushShape;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

VoxelGrid ball(float cell = 0.1f, int radius = 6) {
    VoxelGrid g(cell);
    const std::uint8_t idx = g.palette_add(cf3(0.6f, 0.6f, 0.6f));
    for (int z = -radius; z <= radius; ++z)
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                if (x * x + y * y + z * z <= radius * radius) g.set({x, y, z}, idx);
    return g;
}

BrushParams dab(int size = 9) {
    BrushParams p;
    p.size = size;
    p.shape = BrushShape::Sphere;
    p.falloff = BrushFalloff::Constant;
    p.strength = 1.0f;
    return p;
}

// What a grid CONTAINS, as opposed to what it serialises to — the layer records
// ride in the stream too, and every claim here is about the voxels, not about
// the bookkeeping that produced them. Comparing streams would conflate the two
// and pass for the wrong reason.
std::vector<std::uint8_t> shape_of(const VoxelGrid& g) {
    std::vector<std::uint8_t> cells;
    const std::optional<VoxelCoord> lo = g.bounds_min();
    const std::optional<VoxelCoord> hi = g.bounds_max();
    if (!lo || !hi) return cells;
    // Absolute coordinates, so two grids with different extents cannot compare
    // equal by lining their bounding boxes up.
    for (std::int32_t z = lo->z; z <= hi->z; ++z)
        for (std::int32_t y = lo->y; y <= hi->y; ++y)
            for (std::int32_t x = lo->x; x <= hi->x; ++x) {
                const std::uint8_t v = g.get({x, y, z});
                if (!v) continue;
                for (std::int32_t c : {x, y, z})
                    for (int b = 0; b < 4; ++b)
                        cells.push_back(static_cast<std::uint8_t>(c >> (b * 8)));
                cells.push_back(v);
            }
    return cells;
}

}  // namespace

TEST_CASE("a layer at 0 is the grid without it, and at 1 the pass applied directly") {
    // The two ends of the slider, and both are EXACT rather than close: the
    // dither passes nothing at 0 and everything at 1 by construction.
    VoxelGrid layered = ball();
    const std::vector<std::uint8_t> before = shape_of(layered);

    // The same pass, applied directly to a second grid with no layer at all.
    VoxelGrid direct = ball();
    direct.sculpt_inflate({0, 6, 0}, dab(), 2);
    const std::vector<std::uint8_t> after = shape_of(direct);
    REQUIRE(before != after);  // the pass did something

    layered.begin_sculpt_layer("inflate");
    layered.sculpt_inflate({0, 6, 0}, dab(), 2);
    layered.end_sculpt_layer();
    REQUIRE(layered.sculpt_layer_count() == 1);
    CHECK(layered.sculpt_layer_cell_count(0) > 0);

    CHECK(layered.set_sculpt_layer_strength(0, 0.0f));
    CHECK(shape_of(layered) == before);
    CHECK(layered.set_sculpt_layer_strength(0, 1.0f));
    CHECK(shape_of(layered) == after);
    // ...and back again, as many times as asked.
    CHECK(layered.set_sculpt_layer_strength(0, 0.0f));
    CHECK(shape_of(layered) == before);
}

TEST_CASE("hiding a layer is the grid without it, and showing it restores") {
    VoxelGrid g = ball();
    const std::vector<std::uint8_t> before = shape_of(g);
    g.begin_sculpt_layer("pass");
    g.sculpt_inflate({0, 6, 0}, dab(), 2);
    g.end_sculpt_layer();
    const std::vector<std::uint8_t> after = shape_of(g);

    CHECK(g.set_sculpt_layer_visible(0, false));
    CHECK(shape_of(g) == before);
    CHECK(g.set_sculpt_layer_visible(0, true));
    CHECK(shape_of(g) == after);
}

TEST_CASE("a fractional strength is reproducible and monotone") {
    // Partial strength on binary occupancy means a reproducible fraction of the
    // CELLS. Reproducible is the part a platform could get wrong, and monotone
    // is the part a naive re-seed would: dialling up must ADD cells to the ones
    // already there, not reshuffle which ones are chosen.
    auto at = [](float s) {
        VoxelGrid g = ball();
        g.begin_sculpt_layer("pass");
        g.sculpt_inflate({0, 6, 0}, dab(), 2);
        g.end_sculpt_layer();
        g.set_sculpt_layer_strength(0, s);
        return g;
    };

    // Same strength, same answer — twice, from scratch.
    CHECK(shape_of(at(0.4f)) == shape_of(at(0.4f)));
    CHECK(shape_of(at(0.7f)) == shape_of(at(0.7f)));

    // ...and dialling one grid up gives the same result as building it there.
    VoxelGrid dialled = ball();
    dialled.begin_sculpt_layer("pass");
    dialled.sculpt_inflate({0, 6, 0}, dab(), 2);
    dialled.end_sculpt_layer();
    dialled.set_sculpt_layer_strength(0, 0.2f);
    dialled.set_sculpt_layer_strength(0, 0.9f);
    dialled.set_sculpt_layer_strength(0, 0.7f);
    CHECK(shape_of(dialled) == shape_of(at(0.7f)));

    // Monotone in cell count between the ends.
    const std::size_t none = at(0.0f).occupied_count();
    const std::size_t some = at(0.5f).occupied_count();
    const std::size_t all = at(1.0f).occupied_count();
    CAPTURE(none);
    CAPTURE(some);
    CAPTURE(all);
    CHECK(none <= some);
    CHECK(some <= all);
    CHECK(none < all);  // the pass is not a no-op
}

TEST_CASE("two passes over the same cells compose in order, and the top one wins") {
    // Order is meaningful and this pins WHICH order: layers composite bottom
    // up, so the later pass is the one whose result survives where they
    // overlap.
    VoxelGrid g = ball();
    const std::uint8_t red = g.palette_add(cf3(0.9f, 0.1f, 0.1f));
    const std::uint8_t blue = g.palette_add(cf3(0.1f, 0.1f, 0.9f));
    const VoxelCoord probe{0, 3, 0};

    g.begin_sculpt_layer("red");
    g.set(probe, red);
    g.end_sculpt_layer();
    g.begin_sculpt_layer("blue");
    g.set(probe, blue);
    g.end_sculpt_layer();
    REQUIRE(g.sculpt_layer_count() == 2);
    CHECK(g.get(probe) == blue);  // the top layer wins

    // Turn the top one off and the one below shows through.
    CHECK(g.set_sculpt_layer_visible(1, false));
    CHECK(g.get(probe) == red);
    // Turn BOTH off and the original is back.
    CHECK(g.set_sculpt_layer_visible(0, false));
    CHECK(g.get(probe) != red);
    CHECK(g.get(probe) != blue);
    // And restoring them replays in order.
    CHECK(g.set_sculpt_layer_visible(0, true));
    CHECK(g.set_sculpt_layer_visible(1, true));
    CHECK(g.get(probe) == blue);
}

TEST_CASE("an old pass is adjusted without losing the work that came after") {
    // The claim that separates this from undo, stated as its own case: lower
    // the FIRST of several passes and the later ones are untouched.
    VoxelGrid g = ball();
    const std::uint8_t a = g.palette_add(cf3(0.9f, 0.2f, 0.2f));
    const std::uint8_t b = g.palette_add(cf3(0.2f, 0.9f, 0.2f));
    const std::uint8_t c = g.palette_add(cf3(0.2f, 0.2f, 0.9f));

    // Three passes, each on cells the others do not touch.
    g.begin_sculpt_layer("first");
    for (std::int32_t x = 0; x < 40; ++x) g.set({x, 8, 0}, a);
    g.end_sculpt_layer();
    g.begin_sculpt_layer("second");
    for (std::int32_t x = 0; x < 40; ++x) g.set({x, 9, 0}, b);
    g.end_sculpt_layer();
    g.begin_sculpt_layer("third");
    for (std::int32_t x = 0; x < 40; ++x) g.set({x, 10, 0}, c);
    g.end_sculpt_layer();

    auto row_count = [&g](std::int32_t y, std::uint8_t value) {
        std::size_t n = 0;
        for (std::int32_t x = 0; x < 40; ++x)
            if (g.get({x, y, 0}) == value) ++n;
        return n;
    };
    REQUIRE(row_count(8, a) == 40);
    REQUIRE(row_count(9, b) == 40);
    REQUIRE(row_count(10, c) == 40);

    // Lower the FIRST pass. It contributes proportionally...
    CHECK(g.set_sculpt_layer_strength(0, 0.5f));
    const std::size_t partial = row_count(8, a);
    CHECK(partial > 0);
    CHECK(partial < 40);
    // ...and the two made after it are exactly as they were.
    CHECK(row_count(9, b) == 40);
    CHECK(row_count(10, c) == 40);

    // All the way off, and the later work still stands.
    CHECK(g.set_sculpt_layer_strength(0, 0.0f));
    CHECK(row_count(8, a) == 0);
    CHECK(row_count(9, b) == 40);
    CHECK(row_count(10, c) == 40);

    // And back, exactly.
    CHECK(g.set_sculpt_layer_strength(0, 1.0f));
    CHECK(row_count(8, a) == 40);
}

TEST_CASE("a reader that does not know sculpt layers gets the flattened grid") {
    // The backward-open claim. An older build meets the sculpt tag where it
    // expects the end of the stream and stops there, so what it decodes is the
    // voxel content the layers composed to — the sculpt itself, just no longer
    // adjustable. Simulated by cutting the stream at the tail, which is
    // exactly the prefix such a reader consumes.
    VoxelGrid g = ball();
    g.begin_sculpt_layer("a pass");
    g.sculpt_inflate({0, 6, 0}, dab(), 2);
    g.end_sculpt_layer();
    g.set_sculpt_layer_strength(0, 0.5f);
    const std::vector<std::uint8_t> composed = shape_of(g);

    const std::vector<std::uint8_t> full = g.serialize();
    // The same grid with its layer merged away has no tail at all, so its
    // length is where the tail starts.
    VoxelGrid flat = ball();
    flat.sculpt_inflate({0, 6, 0}, dab(), 2);
    const std::vector<std::uint8_t> no_layers = flat.serialize();
    REQUIRE(full.size() > no_layers.size());

    std::optional<VoxelGrid> old_reader =
        VoxelGrid::deserialize(full.data(), no_layers.size());
    REQUIRE(old_reader.has_value());
    CHECK(old_reader->sculpt_layer_count() == 0);   // it cannot see them
    CHECK(shape_of(*old_reader) == composed);       // but the sculpt is all there
}

TEST_CASE("removing a layer leaves the grid as though the pass never happened") {
    VoxelGrid g = ball();
    const std::vector<std::uint8_t> before = shape_of(g);

    g.begin_sculpt_layer("first");
    g.sculpt_inflate({0, 6, 0}, dab(), 2);
    g.end_sculpt_layer();
    const std::vector<std::uint8_t> after_first = shape_of(g);
    REQUIRE(after_first != before);

    // A second pass somewhere the first one does not reach.
    g.begin_sculpt_layer("second");
    g.sculpt_inflate({0, -6, 0}, dab(), 2);
    g.end_sculpt_layer();

    // Drop the FIRST, keeping the second: everything above replays on top.
    CHECK(g.remove_sculpt_layer(0));
    CHECK(g.sculpt_layer_count() == 1);
    CHECK(g.sculpt_layer_name(0) == "second");

    // The same as having only ever made the second pass — which holds because
    // the two do not touch. See below for what happens when they do.
    VoxelGrid only_second = ball();
    only_second.sculpt_inflate({0, -6, 0}, dab(), 2);
    CHECK(shape_of(g) == shape_of(only_second));

    // And dropping the last one returns the original.
    CHECK(g.remove_sculpt_layer(0));
    CHECK(g.sculpt_layer_count() == 0);
    CHECK(shape_of(g) == before);
}

TEST_CASE("a layer replays what its pass DID, it does not re-run the brush") {
    // The semantic that the previous case's disjoint passes hide, and it is not
    // an implementation leak — it is what a delta stack means, and what ZBrush's
    // layers do. A layer stores the cells its pass changed, so a pass whose
    // result DEPENDED on the layer below (smooth reads its neighbours; inflate
    // grows from what is already there) keeps the result it recorded when the
    // layer below is dialled away. Re-running the brush instead would make
    // every strength change re-evaluate the whole stack, and would make a
    // layer's content depend on what is under it — neither is what a layer is
    // for.
    //
    // Pinning it here so a future change that "fixes" it has to argue with a
    // test rather than with a comment.
    VoxelGrid stacked = ball();
    stacked.begin_sculpt_layer("lower");
    stacked.sculpt_inflate({0, 6, 0}, dab(), 2);
    stacked.end_sculpt_layer();
    stacked.begin_sculpt_layer("upper");
    stacked.sculpt_smooth({3, 5, 0}, dab(11));  // overlaps the lower pass
    stacked.end_sculpt_layer();

    CHECK(stacked.remove_sculpt_layer(0));
    VoxelGrid rerun = ball();
    rerun.sculpt_smooth({3, 5, 0}, dab(11));
    // Deliberately NOT equal: the upper layer kept what it recorded.
    CHECK(shape_of(stacked) != shape_of(rerun));

    // What IS guaranteed: the replay is exact and repeatable. Dial the
    // surviving layer off and on, and it lands back on the same cells.
    const std::vector<std::uint8_t> replayed = shape_of(stacked);
    CHECK(stacked.set_sculpt_layer_strength(0, 0.0f));
    CHECK(stacked.set_sculpt_layer_strength(0, 1.0f));
    CHECK(shape_of(stacked) == replayed);
}

TEST_CASE("merging down folds two passes into one that still dials") {
    VoxelGrid g = ball();
    const std::vector<std::uint8_t> before = shape_of(g);

    g.begin_sculpt_layer("lower");
    g.sculpt_inflate({0, 6, 0}, dab(), 2);
    g.end_sculpt_layer();
    g.begin_sculpt_layer("upper");
    g.sculpt_inflate({6, 0, 0}, dab(), 2);
    g.end_sculpt_layer();
    const std::vector<std::uint8_t> both = shape_of(g);

    CHECK(g.merge_sculpt_layer_down(1));
    CHECK(g.sculpt_layer_count() == 1);
    CHECK(g.sculpt_layer_name(0) == "lower");  // the lower layer keeps its name
    // The visible result is unchanged by merging.
    CHECK(shape_of(g) == both);

    // And the merged layer is still one pass that can be dialled to nothing.
    CHECK(g.set_sculpt_layer_strength(0, 0.0f));
    CHECK(shape_of(g) == before);
    CHECK(g.set_sculpt_layer_strength(0, 1.0f));
    CHECK(shape_of(g) == both);
}

TEST_CASE("sculpt layers survive the file, and a grid without them is unchanged") {
    VoxelGrid g = ball();
    g.begin_sculpt_layer("a pass");
    g.sculpt_inflate({0, 6, 0}, dab(), 2);
    g.end_sculpt_layer();
    g.set_sculpt_layer_strength(0, 0.6f);

    const std::vector<std::uint8_t> bytes = g.serialize();
    std::optional<VoxelGrid> back = VoxelGrid::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    REQUIRE(back->sculpt_layer_count() == 1);
    CHECK(back->sculpt_layer_name(0) == "a pass");
    CHECK(back->sculpt_layer_strength(0) == doctest::Approx(0.6f));
    CHECK(back->sculpt_layer_cell_count(0) == g.sculpt_layer_cell_count(0));
    CHECK(back->serialize() == bytes);  // canonical

    // The reloaded layer still dials, which is the point of storing the diff
    // rather than the result.
    CHECK(back->set_sculpt_layer_strength(0, 1.0f));
    CHECK(g.set_sculpt_layer_strength(0, 1.0f));
    CHECK(back->serialize() == g.serialize());

    // A grid with NO layers pays nothing for the feature existing.
    VoxelGrid plain = ball();
    const std::vector<std::uint8_t> plain_bytes = plain.serialize();
    std::optional<VoxelGrid> plain_back =
        VoxelGrid::deserialize(plain_bytes.data(), plain_bytes.size());
    REQUIRE(plain_back.has_value());
    CHECK(plain_back->sculpt_layer_count() == 0);
    CHECK(plain_back->serialize() == plain_bytes);
}

TEST_CASE("a layer records the pass, not the strokes that follow it") {
    // end_sculpt_layer stops the attribution: work after it belongs to nobody
    // and is not dialled away with the layer.
    VoxelGrid g = ball();
    g.begin_sculpt_layer("pass");
    g.sculpt_inflate({0, 6, 0}, dab(), 2);
    g.end_sculpt_layer();
    CHECK(!g.recording_sculpt_layer());

    const std::uint8_t mark = g.palette_add(cf3(0.2f, 0.8f, 0.2f));
    const VoxelCoord after_cell{5, 5, 5};
    g.set(after_cell, mark);

    // Dialling the layer to zero must not take the later edit with it.
    CHECK(g.set_sculpt_layer_strength(0, 0.0f));
    CHECK(g.get(after_cell) == mark);
    CHECK(g.sculpt_layer_cell_count(0) > 0);
}

TEST_CASE("reordering two passes changes which one wins") {
    // Order is the reason reorder exists rather than a caveat about it: layers
    // composite bottom-up, so moving one past another swaps which value
    // survives where they overlap.
    VoxelGrid g = ball();
    const std::uint8_t red = g.palette_add(cf3(0.9f, 0.1f, 0.1f));
    const std::uint8_t blue = g.palette_add(cf3(0.1f, 0.1f, 0.9f));
    const VoxelCoord probe{0, 3, 0};

    g.begin_sculpt_layer("red");
    g.set(probe, red);
    g.end_sculpt_layer();
    g.begin_sculpt_layer("blue");
    g.set(probe, blue);
    g.end_sculpt_layer();
    REQUIRE(g.get(probe) == blue);

    // Move the red pass to the top and it is the one that shows.
    CHECK(g.move_sculpt_layer(0, 1));
    CHECK(g.sculpt_layer_name(0) == "blue");
    CHECK(g.sculpt_layer_name(1) == "red");
    CHECK(g.get(probe) == red);

    // ...and back.
    CHECK(g.move_sculpt_layer(1, 0));
    CHECK(g.sculpt_layer_name(0) == "red");
    CHECK(g.get(probe) == blue);

    // A move to where it already is changes nothing; a move out of range is
    // refused rather than clamped, since clamping would silently reorder.
    CHECK(g.move_sculpt_layer(1, 1));
    CHECK(g.get(probe) == blue);
    CHECK(!g.move_sculpt_layer(0, 9));
    CHECK(!g.move_sculpt_layer(9, 0));
    CHECK(g.sculpt_layer_name(0) == "red");
}

TEST_CASE("a layer costs its pass, not the model") {
    // The memory claim the design rests on: a stroke over a hundred cells
    // costs a hundred entries whether the grid holds a thousand voxels or a
    // million.
    VoxelGrid small = ball(0.1f, 4);
    VoxelGrid large = ball(0.1f, 12);
    REQUIRE(large.occupied_count() > 4 * small.occupied_count());

    const VoxelCoord centre{0, 0, 0};
    for (VoxelGrid* g : {&small, &large}) {
        g->begin_sculpt_layer("same pass");
        for (std::int32_t x = 0; x < 32; ++x) g->set({x + centre.x, 0, 0}, 1);
        g->end_sculpt_layer();
    }
    CHECK(small.sculpt_layer_cell_count(0) == large.sculpt_layer_cell_count(0));
    CHECK(small.sculpt_layer_bytes(0) == large.sculpt_layer_bytes(0));
    CHECK(small.sculpt_layer_total_bytes() == small.sculpt_layer_bytes(0));

    // A grid with no layers costs nothing, and an index it does not have
    // reports nothing rather than reading past the end.
    VoxelGrid plain = ball(0.1f, 4);
    CHECK(plain.sculpt_layer_total_bytes() == 0);
    CHECK(plain.sculpt_layer_bytes(0) == 0);

    // Merging down is the release valve the header points a host at: two
    // passes over the same cells become one entry per cell.
    large.begin_sculpt_layer("again");
    for (std::int32_t x = 0; x < 32; ++x) large.set({x, 0, 0}, 2);
    large.end_sculpt_layer();
    const std::size_t stacked = large.sculpt_layer_total_bytes();
    CHECK(large.merge_sculpt_layer_down(1));
    CHECK(large.sculpt_layer_total_bytes() < stacked);
}

TEST_CASE("sculpt layers refuse indices they do not have") {
    VoxelGrid g = ball();
    CHECK(!g.set_sculpt_layer_strength(0, 0.5f));
    CHECK(!g.set_sculpt_layer_visible(0, false));
    CHECK(!g.remove_sculpt_layer(0));
    CHECK(!g.merge_sculpt_layer_down(0));
    CHECK(g.sculpt_layer_name(9) == "");
    CHECK(g.sculpt_layer_cell_count(9) == 0);

    g.begin_sculpt_layer("only");
    g.set({0, 0, 0}, 1);
    g.end_sculpt_layer();
    // The bottom layer has nothing to merge into.
    CHECK(!g.merge_sculpt_layer_down(0));
    // Strength is clamped rather than refused: a slider that overshoots is a
    // caller being a caller, not an error.
    CHECK(g.set_sculpt_layer_strength(0, 5.0f));
    CHECK(g.sculpt_layer_strength(0) == doctest::Approx(1.0f));
    CHECK(g.set_sculpt_layer_strength(0, -2.0f));
    CHECK(g.sculpt_layer_strength(0) == doctest::Approx(0.0f));
}
