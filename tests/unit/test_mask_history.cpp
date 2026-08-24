#include <doctest/doctest.h>

#include <vector>

#include "clay/voxel/mask.h"

// Recording a mask edit so it can be undone (masks-in-the-history, #245).
//
// A mask was the FOURTH representation with no history mechanism — twenty
// mutating entry points and not one command variant — which is why a mask edit
// was a barrier: nothing could reverse it and nothing could replay it.
//
// The test that matters is the LAST one: it walks every mutating method, the
// same way `mask: every mutator moves the revision` does, because a sink on
// `set()` would have recorded two of them and silently missed five.

using namespace clay;
using namespace clay::voxel;

namespace {

MaskField painted(float cell = 0.1f) {
    MaskField m(cell);
    BrushParams p;
    p.size = 5;
    m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);
    return m;
}

// The mask as a comparable value: every painted cell and what it holds.
std::vector<std::pair<VoxelCoord, float>> snapshot(const MaskField& m) {
    std::vector<std::pair<VoxelCoord, float>> out;
    const auto lo = m.bounds_min();
    const auto hi = m.bounds_max();
    if (!lo || !hi) return out;
    for (int z = lo->z; z <= hi->z; ++z)
        for (int y = lo->y; y <= hi->y; ++y)
            for (int x = lo->x; x <= hi->x; ++x) {
                const VoxelCoord c{x, y, z};
                const float v = m.get(c);
                if (v != 0.0f) out.emplace_back(c, v);
            }
    return out;
}

}  // namespace

TEST_CASE("mask history: a step records what changed and reverts it") {
    MaskField m = painted();
    const auto before = snapshot(m);
    REQUIRE(!before.empty());

    REQUIRE(m.begin_step());
    BrushParams p;
    p.size = 3;
    m.paint(VoxelCoord{10, 0, 0}, p, 1.0f);
    const std::vector<MaskField::MaskChange> changes = m.end_step();
    REQUIRE(!changes.empty());
    CHECK(snapshot(m) != before);

    m.revert_changes(changes);
    CHECK(snapshot(m) == before);
    m.reapply_changes(changes);
    CHECK(snapshot(m) != before);
    m.revert_changes(changes);
    CHECK(snapshot(m) == before);
}

TEST_CASE("mask history: a step that changed nothing records nothing") {
    // An undo step that undoes nothing is what this mechanism exists to avoid.
    MaskField m = painted();
    REQUIRE(m.begin_step());
    CHECK(m.end_step().empty());  // nothing touched at all

    // And a mutator that ran but changed no cell.
    REQUIRE(m.begin_step());
    m.fill(math::Aabb{kernel::cf3(50, 50, 50), kernel::cf3(51, 51, 51)}, 0.0f);
    CHECK(m.end_step().empty());
}

TEST_CASE("mask history: a nested step is refused") {
    MaskField m = painted();
    REQUIRE(m.begin_step());
    CHECK_FALSE(m.begin_step());
    m.end_step();
    CHECK_FALSE(m.recording_step());
}

TEST_CASE("mask history: the record survives a chunk being created and dropped") {
    // A mask releases storage when its last painted cell goes, so "the chunk is
    // gone" is a real outcome rather than an all-zero chunk — and the diff has
    // to walk the UNION of the keys to see it.
    MaskField m(0.1f);
    REQUIRE(m.begin_step());
    m.set(VoxelCoord{0, 0, 0}, 1.0f);   // creates a chunk
    const auto created = m.end_step();
    REQUIRE(created.size() == 1);
    CHECK(created[0].before == 0);
    CHECK(created[0].after != 0);
    CHECK(m.get(VoxelCoord{0, 0, 0}) == doctest::Approx(1.0f));

    REQUIRE(m.begin_step());
    m.set(VoxelCoord{0, 0, 0}, 0.0f);   // drops it
    const auto dropped = m.end_step();
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].after == 0);
    CHECK(m.empty());

    m.revert_changes(dropped);
    CHECK(m.get(VoxelCoord{0, 0, 0}) == doctest::Approx(1.0f));
}

TEST_CASE("mask history: a replay does not record itself") {
    MaskField m = painted();
    REQUIRE(m.begin_step());
    BrushParams p;
    p.size = 3;
    m.paint(VoxelCoord{10, 0, 0}, p, 1.0f);
    const auto changes = m.end_step();

    // Reverting while another step is open must not journal the revert into it,
    // or an undo would grow the thing it is unwinding.
    REQUIRE(m.begin_step());
    m.revert_changes(changes);
    const auto during = m.end_step();
    CHECK(during.empty());
}

TEST_CASE("mask history: the revision still moves on a replay") {
    // A consumer holding a derivation must be told, whether the change came
    // from a brush or from an undo.
    MaskField m = painted();
    REQUIRE(m.begin_step());
    m.set(VoxelCoord{9, 9, 9}, 1.0f);
    const auto changes = m.end_step();

    const std::uint64_t before = m.revision();
    m.revert_changes(changes);
    CHECK(m.revision() != before);
    const std::uint64_t after_revert = m.revision();
    m.reapply_changes(changes);
    CHECK(m.revision() != after_revert);
}

TEST_CASE("mask history: EVERY mutator is recorded, not just the two through set()") {
    // The point of the whole design. `VoxelGrid::set` is the one choke point
    // every voxel verb funnels through; a mask is NOT built that way — only
    // fill and invert_within go through set(), while invert, clear, expand,
    // contract and smooth write chunk data directly. A sink on set() would have
    // recorded two of these and silently missed five.
    //
    // This walks every mutating method, exactly as the revision test does, so
    // adding a mutator without a case here is the mistake it exists to make
    // loud.
    const auto records = [](const char* what, auto&& mutate) {
        MaskField m = painted();
        const auto before = snapshot(m);
        REQUIRE(m.begin_step());
        mutate(m);
        const std::vector<MaskField::MaskChange> changes = m.end_step();
        CAPTURE(what);
        CHECK_MESSAGE(!changes.empty(), "the mutator changed nothing, so it proves nothing here");
        m.revert_changes(changes);
        CHECK_MESSAGE(snapshot(m) == before, "reverting the record did not restore the mask");
    };

    BrushParams p;
    p.size = 3;
    const math::Aabb region{kernel::cf3(-0.2f, -0.2f, -0.2f), kernel::cf3(0.2f, 0.2f, 0.2f)};

    records("set", [](MaskField& m) { m.set(VoxelCoord{20, 20, 20}, 1.0f); });
    records("paint(world)", [&](MaskField& m) { m.paint(kernel::cf3(1, 0, 0), p, 1.0f); });
    records("paint(cell)", [&](MaskField& m) { m.paint(VoxelCoord{12, 0, 0}, p, 1.0f); });
    records("invert", [](MaskField& m) { m.invert(); });
    records("clear", [](MaskField& m) { m.clear(); });
    records("expand", [](MaskField& m) { m.expand(1); });
    records("contract", [](MaskField& m) { m.contract(1); });
    records("smooth", [](MaskField& m) { m.smooth(1); });
    records("fill", [&](MaskField& m) { m.fill(region, 0.5f); });
    records("invert_within", [&](MaskField& m) { m.invert_within(region); });
}
