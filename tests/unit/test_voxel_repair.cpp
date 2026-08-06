// The remaining voxel verbs and repair (voxel-engine spec,
// add-voxel-verbs + add-voxel-repair).

#include <doctest/doctest.h>

#include <vector>

#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

using namespace clay;
using namespace clay::voxel;
using kernel::cf3;

namespace {

VoxelGrid slab(int thickness = 3, int half = 8) {
    VoxelGrid g(0.1f);
    std::uint8_t stone = g.palette_add(cf3(0.6f, 0.6f, 0.65f));
    g.fill_box({-half, 0, -half}, {half, thickness - 1, half}, stone);
    return g;
}

// A hollow box: a shell with an empty interior the outside cannot reach.
VoxelGrid hollow_box(int half = 5) {
    VoxelGrid g(0.1f);
    std::uint8_t shell = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    g.fill_box({-half, -half, -half}, {half, half, half}, shell);
    g.fill_box({-half + 1, -half + 1, -half + 1}, {half - 1, half - 1, half - 1}, 0);
    return g;
}

BrushParams brush(int size, float strength = 1.0f) {
    BrushParams p;
    p.size = size;
    p.shape = BrushShape::Cube;
    p.strength = strength;
    return p;
}

}  // namespace

TEST_CASE("verbs: fill cavities fills dents but not through-holes") {
    SUBCASE("a pocket is filled") {
        VoxelGrid g = slab(4);
        // One cell across and two deep: its bottom has five of six face
        // neighbours occupied, which is what "inside a pocket" means here.
        g.fill_box({0, 2, 0}, {0, 3, 0}, 0);
        std::size_t before = g.occupied_count();
        g.sculpt_fill_cavities({0, 2, 0}, brush(9), 2);
        CHECK(g.occupied_count() > before);
        CHECK(g.get({0, 2, 0}) != 0);
        CHECK(g.get({0, 3, 0}) != 0);  // the second pass reaches the mouth
    }

    SUBCASE("a wide shallow dent is not a pocket") {
        // Two cells across and one deep: three occupied face neighbours, below
        // the threshold. This is the line the verb draws, and it is deliberate
        // — smoothing is the verb for surface irregularity, not this one.
        VoxelGrid g = slab(4);
        g.fill_box({-1, 3, -1}, {0, 3, 0}, 0);
        std::vector<std::uint8_t> before = g.serialize();
        g.sculpt_fill_cavities({0, 3, 0}, brush(9), 2);
        CHECK(g.serialize() == before);
    }

    SUBCASE("a through-hole is not filled") {
        VoxelGrid g = slab();
        // Pierced all the way through: its interior cells have at most two
        // occupied face neighbours, so nothing about them says "pocket".
        g.fill_box({-3, 0, -3}, {3, 2, 3}, 0);
        g.sculpt_fill_cavities({0, 1, 0}, brush(15), 1);
        CHECK(g.get({0, 1, 0}) == 0);   // still open
        CHECK(g.get({0, 0, 0}) == 0);
    }

    SUBCASE("an open surface is left alone") {
        VoxelGrid g = slab();
        std::vector<std::uint8_t> before = g.serialize();
        g.sculpt_fill_cavities({0, 5, 0}, brush(9), 1);  // above the slab entirely
        CHECK(g.serialize() == before);
    }
}

TEST_CASE("verbs: scrape flattens and smooths from one snapshot") {
    auto bumpy = []() {
        VoxelGrid g = slab();
        std::uint8_t accent = g.palette_add(cf3(0.8f, 0.5f, 0.3f));
        for (int x = -5; x <= 5; x += 2) g.set({x, 3, 0}, accent);  // a row of spurs
        g.set({0, 4, 0}, accent);
        return g;
    };

    VoxelGrid scraped = bumpy();
    std::size_t before = scraped.occupied_count();
    scraped.sculpt_scrape({0, 3, 0}, brush(13), cf3(0, 1, 0), 0.0f);
    CHECK(scraped.occupied_count() < before);
    CHECK(scraped.get({0, 4, 0}) == 0);  // the tallest spur is gone

    SUBCASE("it is snapshot-consistent, so it repeats exactly") {
        VoxelGrid a = bumpy(), b = bumpy();
        a.sculpt_scrape({0, 3, 0}, brush(13), cf3(0, 1, 0), 0.0f);
        b.sculpt_scrape({0, 3, 0}, brush(13), cf3(0, 1, 0), 0.0f);
        CHECK(a.serialize() == b.serialize());
    }

    SUBCASE("a zero-length normal is a no-op, not a crash") {
        VoxelGrid g = bumpy();
        std::vector<std::uint8_t> before_bytes = g.serialize();
        g.sculpt_scrape({0, 3, 0}, brush(13), cf3(0, 0, 0), 0.0f);
        CHECK(g.serialize() == before_bytes);
    }
}

TEST_CASE("verbs: smudge moves the skin, grab moves the lump") {
    auto block = []() {
        VoxelGrid g(0.1f);
        std::uint8_t stone = g.palette_add(cf3(0.6f, 0.6f, 0.6f));
        g.fill_box({-6, -6, -6}, {6, 6, 6}, stone);
        return g;
    };

    VoxelGrid smudged = block();
    smudged.sculpt_smudge({6, 0, 0}, brush(9), cf3(0.3f, 0, 0));
    VoxelGrid grabbed = block();
    grabbed.sculpt_grab({6, 0, 0}, brush(9), cf3(0.3f, 0, 0), false);

    CHECK(smudged.serialize() != block().serialize());  // it did something
    CHECK(smudged.serialize() != grabbed.serialize());  // and not grab's something

    // The interior is untouched: that is the distinction the spec names.
    for (int x = -6; x <= 3; ++x)
        for (int y = -4; y <= 4; ++y) CHECK(smudged.get({x, y, 0}) != 0);
}

TEST_CASE("verbs: carve with an alpha") {
    auto block = []() {
        VoxelGrid g(0.1f);
        std::uint8_t stone = g.palette_add(cf3(0.6f, 0.6f, 0.6f));
        g.fill_box({-8, -8, -8}, {8, 8, 8}, stone);
        return g;
    };
    const int w = 8, h = 8;

    SUBCASE("the alpha shapes the carve") {
        // Opaque on the +u half, empty on the other.
        std::vector<float> alpha(w * h, 0.0f);
        for (int j = 0; j < h; ++j)
            for (int i = w / 2; i < w; ++i) alpha[j * w + i] = 1.0f;

        VoxelGrid g = block();
        REQUIRE(g.sculpt_carve_alpha({0, 0, 0}, brush(11), alpha.data(), w, h, cf3(0, 0, 1)));

        // The direction is +z, so the stamp's plane is xy; one half loses
        // material and the other does not.
        int carved = 0, kept = 0;
        for (int x = -4; x <= 4; ++x)
            for (int y = -4; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z) (g.get({x, y, z}) == 0 ? carved : kept)++;
        CHECK(carved > 0);
        CHECK(kept > 0);
    }

    SUBCASE("a uniform alpha is the plain carve") {
        std::vector<float> alpha(w * h, 1.0f);
        VoxelGrid a = block(), b = block();
        BrushParams p = brush(11);
        p.seed = 5;
        REQUIRE(a.sculpt_carve_alpha({0, 0, 0}, p, alpha.data(), w, h, cf3(0, 0, 1)));
        b.erase_brush({0, 0, 0}, p);
        // The alpha's square covers the footprint's inscribed square, so the
        // corners a plain cube brush reaches are outside the stamp. Compare
        // where the stamp actually applies.
        for (int x = -3; x <= 3; ++x)
            for (int y = -3; y <= 3; ++y)
                for (int z = -5; z <= 5; ++z)
                    CHECK((a.get({x, y, z}) == 0) == (b.get({x, y, z}) == 0));
    }

    SUBCASE("a malformed alpha is refused") {
        std::vector<float> alpha(w * h, 1.0f);
        VoxelGrid g = block();
        std::vector<std::uint8_t> before = g.serialize();
        CHECK_FALSE(g.sculpt_carve_alpha({0, 0, 0}, brush(11), nullptr, w, h, cf3(0, 0, 1)));
        CHECK_FALSE(g.sculpt_carve_alpha({0, 0, 0}, brush(11), alpha.data(), 0, h, cf3(0, 0, 1)));
        CHECK_FALSE(g.sculpt_carve_alpha({0, 0, 0}, brush(11), alpha.data(), w, 0, cf3(0, 0, 1)));
        CHECK_FALSE(
            g.sculpt_carve_alpha({0, 0, 0}, brush(11), alpha.data(), w, h, cf3(0, 0, 0)));
        CHECK(g.serialize() == before);
    }
}

TEST_CASE("verbs: every new verb honours the mask") {
    MaskField mask(0.1f);
    for (int x = -14; x <= 14; ++x)
        for (int y = -14; y <= 14; ++y)
            for (int z = -14; z <= 14; ++z) mask.set({x, y, z}, 1.0f);

    std::vector<float> alpha(16, 1.0f);
    auto gated = [&](auto&& verb) {
        VoxelGrid a = slab(5), b = slab(5);
        // A one-cell pocket, so fill-cavities has work the threshold accepts.
        a.fill_box({0, 3, 0}, {0, 4, 0}, 0);
        b.fill_box({0, 3, 0}, {0, 4, 0}, 0);
        std::vector<std::uint8_t> before = a.serialize();
        BrushParams masked = brush(9);
        masked.mask = &mask;
        verb(a, masked);
        verb(b, brush(9));
        CHECK(b.serialize() != before);      // the verb does something
        return a.serialize() == before;      // ...but not through the mask
    };

    CHECK(gated([](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_fill_cavities({0, 3, 0}, p, 2);
    }));
    CHECK(gated([](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_scrape({0, 3, 0}, p, cf3(0, 1, 0), 0.0f);
    }));
    CHECK(gated([](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_smudge({0, 4, 0}, p, cf3(0.3f, 0, 0));
    }));
    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_carve_alpha({0, 3, 0}, p, alpha.data(), 4, 4, cf3(0, 1, 0));
    }));
}

// -- repair -------------------------------------------------------------------

TEST_CASE("repair: the report says what is enclosed, without changing anything") {
    SUBCASE("a hollow shell reports its void") {
        VoxelGrid g = hollow_box();
        std::vector<std::uint8_t> before = g.serialize();
        VoxelGrid::RepairReport r = g.repair_report();
        CHECK(r.enclosed_voids == 1);
        CHECK(r.void_cells == 9u * 9u * 9u);  // the 11^3 box minus its shell
        CHECK(r.largest_void == r.void_cells);
        CHECK_FALSE(r.airtight);
        CHECK(g.serialize() == before);  // non-destructive
    }

    SUBCASE("a solid block is airtight") {
        VoxelGrid g(0.1f);
        g.fill_box({-3, -3, -3}, {3, 3, 3}, g.palette_add(cf3(1, 1, 1)));
        VoxelGrid::RepairReport r = g.repair_report();
        CHECK(r.enclosed_voids == 0);
        CHECK(r.airtight);
    }

    SUBCASE("an empty grid encloses nothing") {
        VoxelGrid g(0.1f);
        CHECK(g.repair_report().airtight);
    }

    SUBCASE("a perforated shell is not enclosed") {
        VoxelGrid g = hollow_box();
        g.set({5, 0, 0}, 0);  // one cell punched through the wall
        VoxelGrid::RepairReport r = g.repair_report();
        CHECK(r.enclosed_voids == 0);
        CHECK(r.airtight);  // airtight in this sense: nothing is SEALED in
    }

    SUBCASE("two voids are counted separately") {
        VoxelGrid g(0.1f);
        std::uint8_t stone = g.palette_add(cf3(1, 1, 1));
        g.fill_box({-8, -3, -3}, {8, 3, 3}, stone);
        g.set({-4, 0, 0}, 0);
        g.set({4, 0, 0}, 0);
        VoxelGrid::RepairReport r = g.repair_report();
        CHECK(r.enclosed_voids == 2);
        CHECK(r.void_cells == 2);
        CHECK(r.largest_void == 1);
    }
}

TEST_CASE("repair: close holes seals perforations and never removes material") {
    VoxelGrid g = hollow_box();
    g.set({5, 0, 0}, 0);
    REQUIRE(g.repair_report().enclosed_voids == 0);

    std::vector<VoxelCoord> occupied_before;
    for (int x = -6; x <= 6; ++x)
        for (int y = -6; y <= 6; ++y)
            for (int z = -6; z <= 6; ++z)
                if (g.get({x, y, z}) != 0) occupied_before.push_back({x, y, z});

    g.repair_close_holes(1);
    CHECK(g.repair_report().enclosed_voids == 1);  // the interior is sealed in now
    for (VoxelCoord c : occupied_before) CHECK(g.get(c) != 0);  // nothing was lost

    SUBCASE("a large opening is left alone") {
        VoxelGrid open_box = hollow_box();
        open_box.fill_box({5, -3, -3}, {5, 3, 3}, 0);  // a 7x7 mouth
        open_box.repair_close_holes(1);
        CHECK(open_box.repair_report().enclosed_voids == 0);
    }

    SUBCASE("a radius below one does nothing") {
        VoxelGrid untouched = hollow_box();
        std::vector<std::uint8_t> before = untouched.serialize();
        untouched.repair_close_holes(0);
        CHECK(untouched.serialize() == before);
    }
}

TEST_CASE("repair: fill voids fills the enclosed and spares the open") {
    SUBCASE("an enclosed void is filled and the grid becomes airtight") {
        VoxelGrid g = hollow_box();
        std::size_t before = g.occupied_count();
        g.repair_fill_voids();
        CHECK(g.occupied_count() == before + 9u * 9u * 9u);
        CHECK(g.repair_report().airtight);
    }

    SUBCASE("an open cavity is not filled") {
        VoxelGrid g = hollow_box();
        g.fill_box({5, -3, -3}, {5, 3, 3}, 0);  // open the mouth
        std::size_t before = g.occupied_count();
        g.repair_fill_voids();
        CHECK(g.occupied_count() == before);
    }

    SUBCASE("the fill takes the shell's colour") {
        VoxelGrid g = hollow_box();
        std::uint8_t shell = g.get({5, 5, 5});
        REQUIRE(shell != 0);
        g.repair_fill_voids();
        CHECK(g.get({0, 0, 0}) == shell);
        CHECK(g.palette_size() == 2);  // no new palette entry was invented
    }

    SUBCASE("an empty grid is a no-op") {
        VoxelGrid g(0.1f);
        g.repair_fill_voids();
        CHECK(g.occupied_count() == 0);
    }
}

TEST_CASE("repair: a frozen region is not repaired either") {
    MaskField mask(0.1f);
    for (int x = -6; x <= 6; ++x)
        for (int y = -6; y <= 6; ++y)
            for (int z = -6; z <= 6; ++z) mask.set({x, y, z}, 1.0f);

    VoxelGrid g = hollow_box();
    std::size_t before = g.occupied_count();
    g.repair_fill_voids(&mask);
    CHECK(g.occupied_count() == before);
    CHECK_FALSE(g.repair_report().airtight);

    VoxelGrid perforated = hollow_box();
    perforated.set({5, 0, 0}, 0);
    std::vector<std::uint8_t> bytes = perforated.serialize();
    perforated.repair_close_holes(1, &mask);
    CHECK(perforated.serialize() == bytes);
}
