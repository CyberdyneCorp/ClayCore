#include <doctest/doctest.h>

#include "clay/voxel/groups.h"

// Naming a region of surface (add-surface-groups) — ZBrush's PolyGroups,
// Blender's Face Sets. The library had no such concept on any representation.
//
// The design decision this file exists to protect: ONE world-space lattice
// shared by all three representations, rather than a per-face id on a mesh, a
// second palette channel on a grid, and something else for SDF. The two cases
// at the bottom are the ones that killed the free alternative.

using namespace clay;
using namespace clay::voxel;

namespace {

math::Aabb box(float x0, float y0, float z0, float x1, float y1, float z1) {
    return math::Aabb{kernel::cf3(x0, y0, z0), kernel::cf3(x1, y1, z1)};
}

}  // namespace

TEST_CASE("groups: an empty field costs nothing and answers kNoGroup") {
    // Zero is "no group", so a document without groups behaves exactly as one
    // did before this existed.
    GroupField g(0.1f);
    CHECK(g.empty());
    CHECK(g.at(kernel::cf3(0, 0, 0)) == kNoGroup);
    CHECK(g.cell_count() == 0);
    CHECK(g.ids().empty());
    CHECK(g.visible(kNoGroup));
    CHECK_FALSE(g.any_hidden());

    // Assigning kNoGroup does not allocate.
    g.set(VoxelCoord{5, 5, 5}, kNoGroup);
    CHECK(g.empty());
}

TEST_CASE("groups: a region is assigned and resolved in world units") {
    GroupField g(0.1f);
    g.fill(box(-0.25f, -0.25f, -0.25f, 0.25f, 0.25f, 0.25f), 3);
    CHECK(!g.empty());
    CHECK(g.at(kernel::cf3(0, 0, 0)) == 3);
    CHECK(g.at(kernel::cf3(5, 5, 5)) == kNoGroup);
    CHECK(g.cell_count() > 0);
    CHECK(g.cell_count(3) == g.cell_count());
    CHECK(g.ids() == std::vector<GroupId>{3});
}

TEST_CASE("groups: two adjacent fills do not overlap by a cell") {
    // Membership is decided at the cell CENTRE, the same rule MaskField::fill
    // uses — a region that clips a cell does not claim it.
    GroupField g(0.1f);
    g.fill(box(0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f), 1);
    const std::size_t first = g.cell_count(1);
    g.fill(box(0.5f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f), 2);
    CHECK(g.cell_count(1) == first);  // the second fill took none of the first
    CHECK(g.cell_count(2) > 0);
}

TEST_CASE("groups: hiding is a property of the id, not of the cells") {
    // Which is what makes isolate cheap: one flag, not a rewrite of every cell.
    GroupField g(0.1f);
    g.fill(box(-0.3f, -0.3f, -0.3f, 0.0f, 0.3f, 0.3f), 1);
    g.fill(box(0.0f, -0.3f, -0.3f, 0.3f, 0.3f, 0.3f), 2);
    const std::size_t cells_before = g.cell_count();

    g.set_visible(1, false);
    CHECK_FALSE(g.visible(1));
    CHECK(g.visible(2));
    CHECK(g.any_hidden());
    CHECK(g.cell_count() == cells_before);  // nothing was rewritten

    CHECK(g.point_hidden(kernel::cf3(-0.15f, 0, 0)));
    CHECK_FALSE(g.point_hidden(kernel::cf3(0.15f, 0, 0)));

    g.show_all();
    CHECK(g.visible(1));
    CHECK_FALSE(g.any_hidden());
}

TEST_CASE("groups: isolate shows one and hides the others, but not the unnamed") {
    // Ungrouped surface is not something an artist hid, so isolating a group
    // must not make the rest of the model vanish because it was never named.
    GroupField g(0.1f);
    g.fill(box(-0.3f, -0.3f, -0.3f, -0.1f, 0.3f, 0.3f), 1);
    g.fill(box(0.1f, -0.3f, -0.3f, 0.3f, 0.3f, 0.3f), 2);

    g.isolate(2);
    CHECK_FALSE(g.visible(1));
    CHECK(g.visible(2));
    CHECK(g.visible(kNoGroup));                       // the unnamed rest
    CHECK_FALSE(g.point_hidden(kernel::cf3(5, 5, 5)));  // which is most of space
}

TEST_CASE("groups: hiding kNoGroup is refused") {
    GroupField g(0.1f);
    g.set_visible(kNoGroup, false);
    CHECK(g.visible(kNoGroup));
}

TEST_CASE("groups: reassign merges, and merging away deletes") {
    GroupField g(0.1f);
    g.fill(box(-0.3f, -0.3f, -0.3f, -0.1f, 0.3f, 0.3f), 1);
    g.fill(box(0.1f, -0.3f, -0.3f, 0.3f, 0.3f, 0.3f), 2);
    const std::size_t one = g.cell_count(1);
    const std::size_t two = g.cell_count(2);

    CHECK(g.reassign(1, 2) == one);
    CHECK(g.cell_count(1) == 0);
    CHECK(g.cell_count(2) == one + two);
    CHECK(g.ids() == std::vector<GroupId>{2});

    // Merging into kNoGroup deletes without walking the lattice for it, and
    // takes the group's visibility with it — a hidden id nobody carries would
    // keep hiding a group that no longer exists.
    g.set_visible(2, false);
    CHECK(g.reassign(2, kNoGroup) == one + two);
    CHECK(g.empty());
    CHECK(g.visible(2));
}

TEST_CASE("groups: the revision moves on every mutation") {
    // A consumer holding a derivation must be told, exactly as with a mask.
    GroupField g(0.1f);
    const auto moved = [&](const char* what, auto&& mutate) {
        const std::uint64_t before = g.revision();
        mutate(g);
        CAPTURE(what);
        CHECK(g.revision() != before);
    };
    moved("set", [](GroupField& f) { f.set(VoxelCoord{0, 0, 0}, 1); });
    moved("fill", [&](GroupField& f) { f.fill(box(0, 0, 0, 0.2f, 0.2f, 0.2f), 2); });
    moved("set_visible", [](GroupField& f) { f.set_visible(1, false); });
    moved("isolate", [](GroupField& f) { f.isolate(2); });
    moved("show_all", [](GroupField& f) { f.show_all(); });
    moved("reassign", [](GroupField& f) { f.reassign(2, 1); });
}

TEST_CASE("groups: the lattice addresses negative space the same as positive") {
    GroupField g(0.1f);
    g.set(VoxelCoord{-40, -40, -40}, 7);
    g.set(VoxelCoord{40, 40, 40}, 7);
    CHECK(g.get(VoxelCoord{-40, -40, -40}) == 7);
    CHECK(g.get(VoxelCoord{40, 40, 40}) == 7);
    CHECK(g.cell_count(7) == 2);
    const auto lo = g.bounds_min();
    const auto hi = g.bounds_max();
    REQUIRE(lo);
    REQUIRE(hi);
    CHECK(lo->x == -40);
    CHECK(hi->x == 40);
}

TEST_CASE("groups: THE CASE THAT KILLED THE FREE ANSWER — a group spanning two items") {
    // The alternative design for SDF was "map a surface point to the ITEM that
    // produced it", which costs no storage. An armour panel spanning two items
    // is not an item, so that design cannot express this at all.
    //
    // Here the two halves of the panel are at different places — as two items
    // would be — and they carry ONE id.
    GroupField g(0.05f);
    g.fill(box(-0.4f, 0.0f, -0.2f, -0.1f, 0.3f, 0.2f), 9);  // the part on item A
    g.fill(box(0.1f, 0.0f, -0.2f, 0.4f, 0.3f, 0.2f), 9);    // the part on item B

    CHECK(g.at(kernel::cf3(-0.25f, 0.15f, 0)) == 9);
    CHECK(g.at(kernel::cf3(0.25f, 0.15f, 0)) == 9);
    CHECK(g.ids() == std::vector<GroupId>{9});  // ONE group, two items

    g.set_visible(9, false);
    CHECK(g.point_hidden(kernel::cf3(-0.25f, 0.15f, 0)));
    CHECK(g.point_hidden(kernel::cf3(0.25f, 0.15f, 0)));  // both halves, one flag
}

TEST_CASE("groups: THE OTHER CASE — a group that is PART of one item") {
    // The mirror of the case above, and the second reason a per-item rule
    // fails: a face is part of one sphere, and a rule that can only name whole
    // items cannot name it.
    GroupField g(0.05f);
    // Only the +x hemisphere's front patch, not the whole sphere.
    g.fill(box(0.2f, -0.2f, -0.2f, 0.5f, 0.2f, 0.2f), 4);

    CHECK(g.at(kernel::cf3(0.35f, 0, 0)) == 4);        // in the patch
    CHECK(g.at(kernel::cf3(-0.35f, 0, 0)) == kNoGroup);  // the same sphere, not in it

    g.isolate(4);
    CHECK_FALSE(g.point_hidden(kernel::cf3(0.35f, 0, 0)));
}
