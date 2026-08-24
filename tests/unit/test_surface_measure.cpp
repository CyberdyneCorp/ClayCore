#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/brush/procedural_mask.h"
#include "clay/brush/surface_measure.h"

// MEASURING THE SURFACE AT A POINT (add-claycore-bridge).
//
// The measures existed only as a MaskField, which is the right shape for
// masking and the wrong one for a bake, a vertex colour, or anything asking
// about one point. These cover the per-point form, the two new measures that
// need rays, and the property the refactor exists to guarantee: that the mask
// and the point cannot disagree.

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// Analytic fields, so the right answer is arithmetic rather than a rendering.
auto sphere(float r) {
    return [r](cfloat3 p) { return kernel::clength(p) - r; };
}

// A slab of known thickness, normal +Y. Thickness has an exact answer here.
auto slab(float half) {
    return [half](cfloat3 p) { return std::fabs(p.y) - half; };
}

// Two overlapping spheres: the crease between them is genuinely CONCAVE.
//
// A torus is the obvious cavity fixture and is wrong for it — for major R and
// minor r the mean curvature at the inner ring is 1/r - 1/(R-r), so the tube
// wins and the ring reads CONVEX unless R < 2r. That mistake is pinned by its
// own test below rather than left as a comment.
auto crease() {
    return [](cfloat3 p) {
        const float a = kernel::clength(cf3(p.x - 0.3f, p.y, p.z)) - 0.5f;
        const float b = kernel::clength(cf3(p.x + 0.3f, p.y, p.z)) - 0.5f;
        return std::min(a, b);
    };
}

}  // namespace

TEST_CASE("measure: a sphere reads convex everywhere, and never concave") {
    // The sign convention the whole family rests on: for f = |p| - R the
    // Laplacian at the surface is 2/R, POSITIVE for convex.
    auto f = sphere(0.5f);
    brush::MeasureSettings s;
    s.scale = 0.5f;  // the sphere's own radius, so convexity should saturate
    const cfloat3 on = cf3(0.5f, 0, 0);

    const float convex = brush::measure_at(f, brush::SurfaceMeasure::Convexity, on, s);
    const float cavity = brush::measure_at(f, brush::SurfaceMeasure::Cavity, on, s);
    CHECK(convex > 0.5f);
    CHECK(cavity == 0.0f);
    // Curvature is the magnitude, so it agrees with whichever side is non-zero.
    CHECK(brush::measure_at(f, brush::SurfaceMeasure::Curvature, on, s) == doctest::Approx(convex));
}

TEST_CASE("measure: the crease between two spheres is concave") {
    auto f = crease();
    brush::MeasureSettings s;
    s.scale = 0.2f;
    // Where the two spheres meet: x = 0, and the surface is at the y that puts
    // it on both spheres.
    const float y = std::sqrt(0.5f * 0.5f - 0.3f * 0.3f);
    const cfloat3 in_crease = cf3(0.0f, y, 0.0f);
    REQUIRE(std::fabs(f(in_crease)) < 1e-3f);  // non-degenerate: it IS on the surface

    CHECK(brush::measure_at(f, brush::SurfaceMeasure::Cavity, in_crease, s) > 0.0f);
    CHECK(brush::measure_at(f, brush::SurfaceMeasure::Convexity, in_crease, s) == 0.0f);
}

TEST_CASE("measure: an ordinary torus has no concave inner ring") {
    // Pinning the mistake, not the feature. For major R and minor r the mean
    // curvature at the inner ring is 1/r - 1/(R - r): with R = 0.5 and r = 0.2
    // that is 5 - 3.33 > 0, so the TUBE wins and the ring is convex. A cavity
    // fixture built on a torus measures nothing and passes for the wrong
    // reason.
    auto torus = [](cfloat3 p) {
        const float q = kernel::clength(cf3(p.x, 0.0f, p.z)) - 0.5f;
        return kernel::clength(cf3(q, p.y, 0.0f)) - 0.2f;
    };
    brush::MeasureSettings s;
    s.scale = 0.2f;
    const cfloat3 inner = cf3(0.3f, 0, 0);
    REQUIRE(std::fabs(torus(inner)) < 1e-3f);
    CHECK(brush::measure_at(torus, brush::SurfaceMeasure::Cavity, inner, s) == 0.0f);
}

// -- the two that need rays --------------------------------------------------

TEST_CASE("measure: a crevice is more occluded than open surface") {
    auto f = crease();
    brush::MeasureSettings s;
    s.ray_length = 0.5f;
    s.ray_count = 32;

    const float y = std::sqrt(0.5f * 0.5f - 0.3f * 0.3f);
    const cfloat3 in_crease = cf3(0.0f, y, 0.0f);
    const cfloat3 open = cf3(0.8f, 0.0f, 0.0f);  // the far side of one sphere
    // NON-DEGENERATE: both are on the surface. An occlusion probe in open space
    // returns 0 whatever the implementation does.
    REQUIRE(std::fabs(f(in_crease)) < 1e-3f);
    REQUIRE(std::fabs(f(open)) < 1e-3f);

    const float occluded =
        brush::measure_at(f, brush::SurfaceMeasure::AmbientOcclusion, in_crease, s);
    const float exposed = brush::measure_at(f, brush::SurfaceMeasure::AmbientOcclusion, open, s);
    CHECK(occluded > exposed);
    CHECK(exposed < 0.2f);   // an open convex surface sees mostly sky
    CHECK(occluded > 0.05f);  // and the crease genuinely sees less
}

TEST_CASE("measure: occlusion is reproducible, which is not negotiable") {
    // Every other query in this library returns the same bits on every backend
    // and every run. A hemisphere sample is the first thing that could quietly
    // break that.
    auto f = crease();
    brush::MeasureSettings s;
    s.ray_length = 0.5f;
    s.ray_count = 16;
    s.seed = 12345;

    const float y = std::sqrt(0.5f * 0.5f - 0.3f * 0.3f);
    const cfloat3 p = cf3(0.0f, y, 0.0f);
    const float a = brush::measure_at(f, brush::SurfaceMeasure::AmbientOcclusion, p, s);
    const float b = brush::measure_at(f, brush::SurfaceMeasure::AmbientOcclusion, p, s);
    CHECK(a == b);  // exactly, not approximately

    // And the seed actually does something: a different one gives a different
    // sample set. If this ever passed by accident the reproducibility check
    // above would be vacuous.
    s.seed = 999;
    CHECK(brush::measure_at(f, brush::SurfaceMeasure::AmbientOcclusion, p, s) != a);
}

TEST_CASE("measure: thickness reads the actual thickness of a slab") {
    // An exact answer, which is why a slab is the fixture: a 0.4-thick slab
    // probed over 1.0 must read 0.4.
    auto f = slab(0.2f);
    brush::MeasureSettings s;
    s.ray_length = 1.0f;
    const cfloat3 on_top = cf3(0, 0.2f, 0);
    REQUIRE(std::fabs(f(on_top)) < 1e-4f);

    const float t = brush::measure_at(f, brush::SurfaceMeasure::Thickness, on_top, s);
    CHECK(t == doctest::Approx(0.4f).epsilon(0.05));

    // A thicker slab reads thicker, which is the direction that matters.
    auto thick = slab(0.4f);
    CHECK(brush::measure_at(thick, brush::SurfaceMeasure::Thickness, cf3(0, 0.4f, 0), s) >
          t);
}

TEST_CASE("measure: thickness saturates rather than lying when the probe is too short") {
    auto f = slab(0.5f);  // 1.0 thick
    brush::MeasureSettings s;
    s.ray_length = 0.2f;  // far too short to cross it
    CHECK(brush::measure_at(f, brush::SurfaceMeasure::Thickness, cf3(0, 0.5f, 0), s) == 1.0f);
}

// -- the property the refactor exists for ------------------------------------

TEST_CASE("measure: the mask and the point agree about the same surface") {
    // `mask_from_surface` used to carry its own copy of the stencil. One
    // implementation now serves both, and this is what would fail if a second
    // ever reappeared.
    auto f = sphere(0.5f);
    brush::ProceduralMaskSettings ps;
    ps.cell_size = 0.02f;
    ps.band = 0.05f;
    ps.region = math::Aabb{cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f)};
    ps.measure.scale = 0.5f;

    const voxel::MaskField mask =
        brush::mask_from_surface(f, brush::SurfaceMeasure::Convexity, ps);
    REQUIRE(mask.painted_count() > 100);  // non-degenerate: the mask has content

    // The lattice fills in its own defaults for h and scale, so the point form
    // is asked with the same ones — comparing against different settings would
    // measure the defaulting, not the agreement.
    brush::MeasureSettings ms = ps.measure;
    ms.h = ps.cell_size;

    std::size_t compared = 0;
    for (float a = 0.0f; a < 6.28f; a += 0.35f) {
        const cfloat3 p = cf3(0.5f * std::cos(a), 0.5f * std::sin(a), 0.0f);
        const float at_point = brush::measure_at(f, brush::SurfaceMeasure::Convexity, p, ms);
        // The mask quantises to a cell, so compare against the cell the point
        // is in rather than against the point — the quantisation is the
        // lattice's, and it is documented.
        const float in_mask = mask.sample(p);
        CHECK(in_mask == doctest::Approx(at_point).epsilon(0.05));
        ++compared;
    }
    REQUIRE(compared > 10);
}

TEST_CASE("measure: a batch matches the same points measured one at a time") {
    auto f = sphere(0.5f);
    brush::MeasureSettings s;
    s.scale = 0.5f;
    std::vector<cfloat3> pts;
    for (float a = 0.0f; a < 6.28f; a += 0.2f)
        pts.push_back(cf3(0.5f * std::cos(a), 0.5f * std::sin(a), 0.0f));
    REQUIRE(pts.size() > 20);

    std::vector<float> batch(pts.size(), -1.0f);
    brush::measure_points(f, brush::SurfaceMeasure::Convexity, pts.data(), pts.size(), s,
                          batch.data());
    for (std::size_t i = 0; i < pts.size(); ++i)
        CHECK(batch[i] ==
              doctest::Approx(brush::measure_at(f, brush::SurfaceMeasure::Convexity, pts[i], s)));
}
