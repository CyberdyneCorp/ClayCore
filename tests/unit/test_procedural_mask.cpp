#include <doctest/doctest.h>

#include <cmath>

#include "clay/brush/procedural_mask.h"

// Masks derived from the surface (procedural masks, roadmap P1).
//
// Every mask verb before this was something an artist DOES — paint, fill,
// expand, smooth. Nothing derived a mask from the SHAPE, so "mask the crevices"
// and "mask everything facing up" had to be painted by hand.
//
// The sign convention is the thing to pin: for a distance field the Laplacian
// at the surface is POSITIVE where the surface is convex (2/R for a sphere of
// radius R). Every test below is really a test of that.

using namespace clay;
using namespace clay::brush;

namespace {

auto sphere(float r) {
    return [r](kernel::cfloat3 p) { return kernel::clength(p) - r; };
}

// Two overlapping spheres. The crease where they meet is THE cavity case — it
// is what "the seam where two forms meet" means, and it is what an artist is
// pointing at when they reach for cavity masking.
auto crease(float r, float offset) {
    return [r, offset](kernel::cfloat3 p) {
        const float a = kernel::clength(kernel::cf3(p.x - offset, p.y, p.z)) - r;
        const float b = kernel::clength(kernel::cf3(p.x + offset, p.y, p.z)) - r;
        return std::min(a, b);
    };
}

// A torus whose HOLE is tighter than its tube. The distinction matters and is
// easy to get wrong: mean curvature is 1/minor - 1/(major - minor), so an
// ORDINARY torus (major 0.4, minor 0.15) is convex everywhere — the tube wins.
// A cavity needs major < 2 * minor. The first draft of this file used the
// ordinary one and asserted a cavity that is not there.
auto fat_torus(float major, float minor) {
    return [major, minor](kernel::cfloat3 p) {
        const float q = std::sqrt(p.x * p.x + p.z * p.z) - major;
        return std::sqrt(q * q + p.y * p.y) - minor;
    };
}

ProceduralMaskSettings around(float extent, float cell, float scale = 0.05f) {
    ProceduralMaskSettings s;
    s.region = math::Aabb{kernel::cf3(-extent, -extent, -extent),
                          kernel::cf3(extent, extent, extent)};
    s.cell_size = cell;
    s.scale = scale;
    return s;
}

}  // namespace

TEST_CASE("procedural mask: a sphere reads CONVEX everywhere, never as a cavity") {
    // The sign convention, stated as a shape everyone agrees about. A sphere is
    // convex at every point; if cavity found anything here the sign is flipped.
    const auto f = sphere(0.5f);
    const ProceduralMaskSettings s = around(0.7f, 0.02f, 0.5f);

    const voxel::MaskField convex = mask_from_surface(f, SurfaceMeasure::Convexity, s);
    const voxel::MaskField cavity = mask_from_surface(f, SurfaceMeasure::Cavity, s);

    CHECK(convex.painted_count() > 0);
    CHECK(cavity.painted_count() == 0);
}

TEST_CASE("procedural mask: the crease between two forms reads as a CAVITY") {
    // The counterpart to the sphere, and the case cavity masking exists for.
    const auto f = crease(0.3f, 0.2f);
    ProceduralMaskSettings s = around(0.8f, 0.015f, 0.1f);
    s.band = 0.03f;

    const voxel::MaskField cavity = mask_from_surface(f, SurfaceMeasure::Cavity, s);
    REQUIRE(cavity.painted_count() > 0);

    // The crease circle sits at x = 0, radius sqrt(r^2 - offset^2).
    const float ring = std::sqrt(0.3f * 0.3f - 0.2f * 0.2f);
    CHECK(cavity.sample(kernel::cf3(0.0f, ring, 0.0f)) > 0.0f);
    // And NOT on the far side of either sphere, which is plain convex.
    CHECK(cavity.sample(kernel::cf3(0.5f, 0.0f, 0.0f)) == doctest::Approx(0.0f));
}

TEST_CASE("procedural mask: an ordinary torus is convex everywhere, a fat one is not") {
    // Worth pinning because it is genuinely counter-intuitive and the first
    // draft of this file got it wrong. Mean curvature on a torus is
    // 1/minor - 1/(major - minor): the TUBE competes with the HOLE, and the
    // tube wins unless major < 2 * minor.
    ProceduralMaskSettings s = around(0.8f, 0.015f, 0.1f);
    s.band = 0.03f;

    const voxel::MaskField ordinary =
        mask_from_surface(fat_torus(0.4f, 0.15f), SurfaceMeasure::Cavity, s);
    CHECK(ordinary.painted_count() == 0);  // 1/0.15 > 1/0.25: convex everywhere

    const voxel::MaskField fat =
        mask_from_surface(fat_torus(0.25f, 0.2f), SurfaceMeasure::Cavity, s);
    CHECK(fat.painted_count() > 0);        // 1/0.2 < 1/0.05: the hole wins
}

TEST_CASE("procedural mask: curvature finds both, cavity and convexity partition it") {
    const auto f = crease(0.3f, 0.2f);
    ProceduralMaskSettings s = around(0.8f, 0.015f, 0.1f);
    s.band = 0.03f;

    const voxel::MaskField all = mask_from_surface(f, SurfaceMeasure::Curvature, s);
    const voxel::MaskField cavity = mask_from_surface(f, SurfaceMeasure::Cavity, s);
    const voxel::MaskField convex = mask_from_surface(f, SurfaceMeasure::Convexity, s);

    CHECK(all.painted_count() > 0);
    CHECK(cavity.painted_count() > 0);
    CHECK(convex.painted_count() > 0);
    // |k| is the union of the two halves, so neither can exceed it and the two
    // together account for it. Saturation means this is <=, not ==.
    CHECK(cavity.painted_count() <= all.painted_count());
    CHECK(convex.painted_count() <= all.painted_count());
}

TEST_CASE("procedural mask: a tighter feature masks more strongly than a gentle one") {
    // `scale` is the RADIUS that reads as fully masked, and curvature is
    // 1/radius — so a small sphere must read stronger than a large one at the
    // same setting. If this inverted, the parameter would mean the opposite of
    // what it says.
    ProceduralMaskSettings s = around(1.2f, 0.02f, 0.2f);
    const voxel::MaskField tight = mask_from_surface(sphere(0.15f), SurfaceMeasure::Convexity, s);
    const voxel::MaskField gentle = mask_from_surface(sphere(1.0f), SurfaceMeasure::Convexity, s);

    REQUIRE(tight.painted_count() > 0);
    REQUIRE(gentle.painted_count() > 0);
    CHECK(tight.sample(kernel::cf3(0.15f, 0, 0)) > gentle.sample(kernel::cf3(1.0f, 0, 0)));
}

TEST_CASE("procedural mask: normal direction masks a hemisphere, and the threshold narrows it") {
    const auto f = sphere(0.5f);
    ProceduralMaskSettings s = around(0.7f, 0.02f);
    s.direction = kernel::cf3(0, 1, 0);

    const voxel::MaskField up = mask_from_surface(f, SurfaceMeasure::NormalDirection, s);
    REQUIRE(up.painted_count() > 0);
    CHECK(up.sample(kernel::cf3(0, 0.5f, 0)) > 0.9f);            // the pole, facing up
    CHECK(up.sample(kernel::cf3(0, -0.5f, 0)) == doctest::Approx(0.0f));  // facing down

    // Raising the threshold NARROWS the cone rather than dimming everything,
    // which is what a caller means by a threshold.
    s.threshold = 0.8f;
    const voxel::MaskField cone = mask_from_surface(f, SurfaceMeasure::NormalDirection, s);
    CHECK(cone.painted_count() < up.painted_count());
    CHECK(cone.sample(kernel::cf3(0, 0.5f, 0)) > 0.9f);  // the pole is still full
}

TEST_CASE("procedural mask: only the surface is measured, not the interior") {
    // A measure taken deep inside a solid describes nothing an artist can see,
    // so the band is what keeps a mask about the SURFACE.
    const auto f = sphere(0.5f);
    ProceduralMaskSettings s = around(0.7f, 0.02f, 0.5f);
    s.band = 0.03f;

    const voxel::MaskField m = mask_from_surface(f, SurfaceMeasure::Convexity, s);
    REQUIRE(m.painted_count() > 0);
    CHECK(m.sample(kernel::cf3(0, 0, 0)) == doctest::Approx(0.0f));      // the centre
    CHECK(m.sample(kernel::cf3(0.5f, 0, 0)) > 0.0f);                     // the surface
}

TEST_CASE("procedural mask: an empty or unbounded region yields an empty mask") {
    const auto f = sphere(0.5f);
    ProceduralMaskSettings s;
    s.cell_size = 0.02f;
    CHECK(mask_from_surface(f, SurfaceMeasure::Curvature, s).empty());  // default region is empty

    s.region = math::Aabb{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};
    CHECK(mask_from_surface(nullptr, SurfaceMeasure::Curvature, s).empty());  // no source
}

TEST_CASE("procedural mask: it is cancellable, and a cancel is told apart from empty") {
    // Both come back empty, so the flag is the only thing that distinguishes
    // "the user stopped it" from "there was no surface in the region".
    parallel::CancelToken token;
    token.cancel();
    bool cancelled = false;
    const voxel::MaskField m = mask_from_surface(sphere(0.5f), SurfaceMeasure::Curvature,
                                                 around(0.7f, 0.01f), &token, &cancelled);
    CHECK(m.empty());
    CHECK(cancelled);

    bool not_cancelled = true;
    const voxel::MaskField ok = mask_from_surface(sphere(0.5f), SurfaceMeasure::Curvature,
                                                  around(0.7f, 0.05f), nullptr, &not_cancelled);
    CHECK_FALSE(not_cancelled);
    CHECK(!ok.empty());
}
