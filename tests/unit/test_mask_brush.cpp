// The mask brush (brush-engine, voxel-engine and sdf-kernels specs,
// add-mask-stroke-brush): painting a mask along a stroke, the bounded
// complement, and the freeze finally reaching the field verbs.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/field/flatten.h"
#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/voxel/mask.h"

using namespace clay;
using brush::Accumulation;
using brush::Stamp;
using brush::StrokePreset;
using brush::StrokeSample;
using field::FieldVolume;
using kernel::cf3;
using voxel::MaskField;

namespace {

// A straight drag along X, which is enough for everything here: what is being
// tested is the consumer, and the path's shape is resolve_stroke's business.
std::vector<StrokeSample> drag_x(float from, float to, int samples = 24) {
    std::vector<StrokeSample> out;
    for (int i = 0; i < samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(samples - 1);
        StrokeSample s;
        s.position = cf3(from + (to - from) * t, 0.0f, 0.0f);
        out.push_back(s);
    }
    return out;
}

// The WORLD extent a mask spans on one axis. Two masks at different
// resolutions are compared through this rather than through their painted
// volume: a discretized sphere of 8 cells and one of 16 differ in volume by
// more than the conversion being tested does, so a volume comparison measures
// the lattice rather than the footprint and needs a tolerance loose enough to
// hide the bug. An extent is the width the conversion actually fixes, and it
// agrees to within a cell.
float masked_extent(const MaskField& m, int axis) {
    auto lo = m.bounds_min();
    auto hi = m.bounds_max();
    if (!lo || !hi) return 0.0f;
    const auto on = [](voxel::VoxelCoord c, int a) { return a == 0 ? c.x : (a == 1 ? c.y : c.z); };
    return static_cast<float>(on(*hi, axis) - on(*lo, axis) + 1) * m.cell_size();
}

// A mask reaches a field verb as a callable, which is what keeps a sampled
// field a leaf module: it sits below scene, and a mask sits above it.
field::MaskGate gate(const MaskField& m) {
    return [&m](kernel::cfloat3 p) { return m.sample(p); };
}

FieldVolume sphere_volume(float r = 0.6f, float cell = 0.04f) {
    return FieldVolume::sample([r](kernel::cfloat3 p) { return kernel::clength(p) - r; },
                               math::Aabb(cf3(-1, -1, -1), cf3(1, 1, 1)), cell, 0.16f);
}

}  // namespace

// -- the third consumer -------------------------------------------------------

TEST_CASE("mask brush: a drag paints a band") {
    MaskField m(0.05f);
    StrokePreset preset;
    preset.radius = 0.15f;
    preset.spacing = 0.3f;

    std::vector<Stamp> stamps = brush::resolve_stroke(drag_x(-0.5f, 0.5f), preset);
    REQUIRE(stamps.size() > 1);
    CHECK(brush::apply_to_mask(m, stamps, 1.0f) == stamps.size());

    // Masked along the path...
    for (float x = -0.4f; x <= 0.4f; x += 0.1f)
        CHECK(m.sample(cf3(x, 0.0f, 0.0f)) > 0.5f);
    // ...and not away from it, on either axis.
    CHECK(m.sample(cf3(0.0f, 1.0f, 0.0f)) == doctest::Approx(0.0f));
    CHECK(m.sample(cf3(3.0f, 0.0f, 0.0f)) == doctest::Approx(0.0f));
}

TEST_CASE("mask brush: the mask's resolution does not change the stroke's width") {
    // The regression for the one conversion this consumer exists to own. A
    // caller sizing the footprint in mask cells by hand gets a stroke whose
    // width tracks the mask's resolution instead of the brush's radius.
    StrokePreset preset;
    preset.radius = 0.2f;
    preset.spacing = 0.25f;
    const std::vector<Stamp> stamps = brush::resolve_stroke(drag_x(-0.4f, 0.4f), preset);

    MaskField coarse(0.05f), fine(0.025f);
    brush::apply_to_mask(coarse, stamps, 1.0f);
    brush::apply_to_mask(fine, stamps, 1.0f);

    // Across the stroke, the width IS the brush diameter, and it must not move
    // when the lattice under it changes. Tolerance is one coarse cell, which is
    // the most a lattice can disagree about where an edge falls.
    for (int axis : {1, 2}) {  // Y and Z; the drag runs along X
        const float a = masked_extent(coarse, axis);
        const float b = masked_extent(fine, axis);
        REQUIRE(a > 0.0f);
        CHECK(a == doctest::Approx(preset.radius * 2.0f).epsilon(0.2));
        CHECK(std::abs(a - b) <= coarse.cell_size() + 1e-4f);
    }
    // ...and along it, the path length plus that same diameter.
    CHECK(std::abs(masked_extent(coarse, 0) - masked_extent(fine, 0)) <=
          coarse.cell_size() + 1e-4f);
}

TEST_CASE("mask brush: erasing is the same call") {
    MaskField m(0.05f);
    StrokePreset preset;
    preset.radius = 0.2f;
    const std::vector<Stamp> stamps = brush::resolve_stroke(drag_x(-0.3f, 0.3f), preset);

    brush::apply_to_mask(m, stamps, 1.0f);
    REQUIRE(m.sample(cf3(0, 0, 0)) > 0.9f);

    brush::apply_to_mask(m, stamps, 0.0f);
    CHECK(m.sample(cf3(0, 0, 0)) < 0.1f);
}

TEST_CASE("mask brush: accumulation over a lerp-toward-target field") {
    StrokePreset preset;
    preset.radius = 0.2f;
    preset.spacing = 0.1f;  // heavily overlapping
    preset.strength = 0.3f;
    preset.pressure.strength = 0.0f;  // pressure out of the way

    MaskField buildup(0.05f), clamped(0.05f);

    preset.accumulation = Accumulation::Buildup;
    brush::apply_to_mask(buildup, brush::resolve_stroke(drag_x(-0.3f, 0.3f), preset), 1.0f);
    preset.accumulation = Accumulation::Clamped;
    brush::apply_to_mask(clamped, brush::resolve_stroke(drag_x(-0.3f, 0.3f), preset), 1.0f);

    const float b = buildup.sample(cf3(0, 0, 0));
    const float c = clamped.sample(cf3(0, 0, 0));

    CHECK(b <= 1.0f);  // approaches the target, never past it
    CHECK(b > c);      // and gets further than one pass would
    CHECK(c > 0.0f);
    CHECK(c <= 1.0f);
}

// -- the bounded complement ---------------------------------------------------

TEST_CASE("mask: invert_within masks everything else") {
    MaskField m(0.1f);
    voxel::BrushParams p;
    p.size = 3;
    p.shape = voxel::BrushShape::Sphere;
    m.paint(cf3(0.05f, 0.05f, 0.05f), p, 1.0f);

    const math::Aabb box(cf3(-1, -1, -1), cf3(1, 1, 1));
    m.invert_within(box);

    CHECK(m.sample(cf3(0.05f, 0.05f, 0.05f)) < 0.1f);  // what was painted is released
    CHECK(m.sample(cf3(0.55f, 0.55f, 0.55f)) > 0.9f);  // the rest of the box is frozen
    CHECK(m.sample(cf3(0.95f, 0.0f, 0.0f)) > 0.9f);
}

TEST_CASE("mask: the region's edge is the region's edge, not a chunk's") {
    // invert() flips whole chunks, so a box whose faces fall mid-chunk is
    // exactly where the two forms differ.
    MaskField m(0.1f);
    const math::Aabb box(cf3(-0.35f, -0.35f, -0.35f), cf3(0.35f, 0.35f, 0.35f));
    m.invert_within(box);

    CHECK(m.sample(cf3(0.05f, 0.05f, 0.05f)) > 0.9f);  // inside
    CHECK(m.sample(cf3(0.25f, 0.05f, 0.05f)) > 0.9f);  // still inside
    CHECK(m.sample(cf3(0.45f, 0.05f, 0.05f)) == doctest::Approx(0.0f));  // outside the box
    CHECK(m.sample(cf3(-0.45f, 0.05f, 0.05f)) == doctest::Approx(0.0f));
}

TEST_CASE("mask: fill, and filling with zero empties") {
    MaskField m(0.1f);
    const math::Aabb box(cf3(-0.3f, -0.3f, -0.3f), cf3(0.3f, 0.3f, 0.3f));

    m.fill(box, 0.5f);
    CHECK(m.sample(cf3(0.05f, 0.05f, 0.05f)) == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(m.sample(cf3(0.95f, 0.0f, 0.0f)) == doctest::Approx(0.0f));

    m.fill(box, 0.0f);
    CHECK(m.empty());
}

TEST_CASE("mask: a region that cannot be walked is refused rather than obeyed") {
    MaskField m(0.1f);

    CHECK_FALSE(m.region_is_walkable(math::Aabb::infinite()));
    m.fill(math::Aabb::infinite(), 1.0f);
    CHECK(m.empty());

    CHECK_FALSE(m.region_is_walkable(math::Aabb()));  // empty
    m.invert_within(math::Aabb());
    CHECK(m.empty());

    // Regression: a box near FLT_MAX is not the infinite SENTINEL, so a check
    // for that alone let it through — and its cell indices overflow the
    // lattice's int32 long before its volume overflows a float.
    const float huge = 3.4e38f;
    const math::Aabb enormous(cf3(-huge, -huge, -huge), cf3(huge, huge, huge));
    CHECK_FALSE(enormous.is_infinite());
    CHECK_FALSE(m.region_is_walkable(enormous));
    m.fill(enormous, 1.0f);
    CHECK(m.empty());

    // And one merely far too large on a single axis.
    const math::Aabb slab(cf3(-1e9f, -0.1f, -0.1f), cf3(1e9f, 0.1f, 0.1f));
    CHECK_FALSE(m.region_is_walkable(slab));
    m.invert_within(slab);
    CHECK(m.empty());

    // A region a caller actually means is walkable.
    CHECK(m.region_is_walkable(math::Aabb(cf3(-1, -1, -1), cf3(1, 1, 1))));
}

// -- the freeze reaching the field verbs --------------------------------------

TEST_CASE("mask: relax leaves a frozen region alone") {
    const FieldVolume source = sphere_volume();

    field::RelaxSettings settings;
    settings.region_radius = 0.5f;
    settings.falloff = 0.2f;
    settings.centre = cf3(0.6f, 0, 0);
    settings.iterations = 3;
    settings.radius_cells = 2;

    const FieldVolume unmasked = field::relax(source, settings);

    // Freeze the whole region the relax acts over.
    MaskField m(0.05f);
    m.fill(math::Aabb(cf3(-0.2f, -1.2f, -1.2f), cf3(1.4f, 1.2f, 1.2f)), 1.0f);
    settings.mask = gate(m);
    const FieldVolume masked = field::relax(source, settings);

    const kernel::cfloat3 probe = cf3(0.6f, 0, 0);
    REQUIRE(source.has_samples_at(probe));
    // Frozen means unchanged, not nearly unchanged.
    CHECK(masked.eval(probe) == doctest::Approx(source.eval(probe)));
    // And the same relax without the mask really did move it.
    CHECK(unmasked.eval(probe) != doctest::Approx(source.eval(probe)));
}

TEST_CASE("mask: flatten leaves a frozen region alone, and half masking attenuates") {
    const float r = 0.6f;
    const auto sphere = [r](kernel::cfloat3 p) { return kernel::clength(p) - r; };
    const math::Aabb region(cf3(-1, -1, -1), cf3(1, 1, 1));

    field::FlattenSettings settings;
    settings.plane_point = cf3(0, 0.4f, 0);
    settings.plane_normal = cf3(0, 1, 0);
    settings.centre = cf3(0, r, 0);
    settings.region_radius = 0.4f;
    settings.falloff = 0.2f;

    const FieldVolume open = field::flatten(sphere, region, 0.04f, 0.16f, settings);

    MaskField frozen(0.05f);
    frozen.fill(math::Aabb(cf3(-1, 0.0f, -1), cf3(1, 1, 1)), 1.0f);
    settings.mask = gate(frozen);
    const FieldVolume held = field::flatten(sphere, region, 0.04f, 0.16f, settings);

    MaskField half(0.05f);
    half.fill(math::Aabb(cf3(-1, 0.0f, -1), cf3(1, 1, 1)), 0.5f);
    settings.mask = gate(half);
    const FieldVolume partial = field::flatten(sphere, region, 0.04f, 0.16f, settings);

    // Where the surface ends up on the axis the flatten pushes along.
    const auto surface_y = [](const FieldVolume& v) {
        float last = 1.0f;
        for (float y = 0.9f; y > 0.0f; y -= 0.002f) {
            const float d = v.eval(cf3(0, y, 0));
            if (d <= 0.0f && last > 0.0f) return y;
            last = d;
        }
        return 0.0f;
    };

    const float unmasked_y = surface_y(open);
    const float frozen_y = surface_y(held);
    const float partial_y = surface_y(partial);

    CHECK(frozen_y == doctest::Approx(r).epsilon(0.02));  // where the sphere put it
    CHECK(unmasked_y < r - 0.05f);                        // pulled down onto the plane
    CHECK(partial_y > unmasked_y);                        // less far than unmasked...
    CHECK(partial_y < frozen_y);                          // ...and further than frozen
}
