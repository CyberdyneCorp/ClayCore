// Mask extrude (sdf-kernels and voxel-engine specs, add-mask-extrude): the mask
// measured as a distance, the plate that comes off a surface, and the agreement
// between the two representations that keeps a document meaning one thing.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/brush/mask_extrude.h"
#include "clay/field/volume.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

using namespace clay;
using brush::ExtrudeSide;
using field::FieldVolume;
using brush::MaskExtrudeSettings;
using kernel::cf3;
using kernel::cfloat3;
using voxel::MaskField;
using voxel::VoxelCoord;
using voxel::VoxelGrid;

namespace {

constexpr float kRadius = 0.6f;

auto sphere_field(float r = kRadius) {
    return [r](cfloat3 p) { return kernel::clength(p) - r; };
}

// A cap of the sphere masked from the +Y pole: the plate an extract is for.
MaskField cap_mask(float cell = 0.03f, float cap_radius = 0.3f) {
    MaskField m(cell);
    const cfloat3 pole = cf3(0, kRadius, 0);
    const auto to_cell = [cell](float w) { return static_cast<std::int32_t>(std::floor(w / cell)); };
    const float reach = cap_radius + 0.2f;
    for (std::int32_t z = to_cell(pole.z - reach); z <= to_cell(pole.z + reach); ++z)
        for (std::int32_t y = to_cell(pole.y - reach); y <= to_cell(pole.y + reach); ++y)
            for (std::int32_t x = to_cell(pole.x - reach); x <= to_cell(pole.x + reach); ++x) {
                const cfloat3 c = cf3(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                      static_cast<float>(z) + 0.5f) *
                                  cell;
                if (kernel::clength(c - pole) <= cap_radius) m.set({x, y, z}, 1.0f);
            }
    return m;
}

// A voxelized ball of the same radius, so the two representations can be asked
// the same question.
VoxelGrid ball_grid(float vs = 0.03f, float r = kRadius) {
    VoxelGrid g(vs);
    const std::uint8_t idx = g.palette_add(cf3(0.8f, 0.2f, 0.2f));
    const auto n = static_cast<std::int32_t>(std::ceil(r / vs)) + 2;
    for (std::int32_t z = -n; z <= n; ++z)
        for (std::int32_t y = -n; y <= n; ++y)
            for (std::int32_t x = -n; x <= n; ++x) {
                const cfloat3 c = cf3(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                      static_cast<float>(z) + 0.5f) *
                                  vs;
                if (kernel::clength(c) <= r) g.set({x, y, z}, idx);
            }
    return g;
}

MaskExtrudeSettings plate_settings(float thickness = 0.12f) {
    MaskExtrudeSettings s;
    s.thickness = thickness;
    s.side = ExtrudeSide::Outward;
    return s;
}

// Where the extract's surface sits along +Y, marching in from outside.
float outer_surface_y(const FieldVolume& v) {
    float last = 1.0f;
    for (float y = 1.2f; y > 0.0f; y -= 0.002f) {
        const float d = v.eval(cf3(0, y, 0));
        if (d <= 0.0f && last > 0.0f) return y;
        last = d;
    }
    return 0.0f;
}

float inner_surface_y(const FieldVolume& v) {
    bool seen_inside = false;
    for (float y = 1.2f; y > 0.0f; y -= 0.002f) {
        const float d = v.eval(cf3(0, y, 0));
        if (d <= 0.0f) seen_inside = true;
        if (seen_inside && d > 0.0f) return y;
    }
    return 0.0f;
}

}  // namespace

// -- the mask, measured -------------------------------------------------------

TEST_CASE("mask_to_field: inside is negative, outside is positive, and it is a distance") {
    const MaskField m = cap_mask();
    // A band wide enough to hold real distances either side of the cap's
    // border: past the band a volume reports a bound rather than a distance, so
    // probing out there would be testing FieldVolume's sparsity, not this.
    const std::optional<FieldVolume> d = brush::mask_to_field(m, 0.5f, 0.25f, 0.3f);
    REQUIRE(d.has_value());

    const cfloat3 pole = cf3(0, kRadius, 0);
    CHECK(d->eval(pole) < -0.2f);                    // well inside the cap
    CHECK(d->eval(pole + cf3(0.45f, 0, 0)) > 0.1f);  // well outside it
    CHECK(d->eval(pole + cf3(0.3f, 0, 0)) == doctest::Approx(0.0f).epsilon(0.2));  // its border

    // A distance changes at roughly unit rate, which is the property that lets
    // it enter a field expression at all.
    const float a = d->eval(pole + cf3(0.05f, 0, 0));
    const float b = d->eval(pole + cf3(0.15f, 0, 0));
    CHECK(std::abs((b - a) - 0.10f) < 0.04f);
}

TEST_CASE("mask_to_field: an empty mask converts to nothing") {
    const MaskField empty(0.05f);
    CHECK_FALSE(brush::mask_to_field(empty).has_value());
    // ...and so does one painted only below the threshold.
    MaskField faint(0.05f);
    faint.fill(math::Aabb{cf3(-0.2f, -0.2f, -0.2f), cf3(0.2f, 0.2f, 0.2f)}, 0.2f);
    CHECK_FALSE(brush::mask_to_field(faint, 0.5f).has_value());
}

// -- the extrude, on a field --------------------------------------------------

TEST_CASE("mask extrude: a plate comes off a sphere") {
    const MaskField m = cap_mask();
    const MaskExtrudeSettings s = plate_settings(0.12f);
    const std::optional<FieldVolume> plate = brush::mask_extrude(sphere_field(), m, s);
    REQUIRE(plate.has_value());

    // It sits ON the surface, and it is as thick as it was asked to be.
    const float outer = outer_surface_y(*plate);
    const float inner = inner_surface_y(*plate);
    CHECK(inner == doctest::Approx(kRadius).epsilon(0.06));
    CHECK(outer - inner == doctest::Approx(s.thickness).epsilon(0.25));

    // And nothing away from the mask: the far side of the sphere is untouched.
    CHECK(plate->eval(cf3(0, -kRadius, 0)) > 0.0f);
    CHECK(plate->eval(cf3(kRadius, 0, 0)) > 0.0f);
}

TEST_CASE("mask extrude: each side means what it says") {
    const MaskField m = cap_mask();
    const auto source = sphere_field();

    MaskExtrudeSettings s = plate_settings(0.12f);
    s.side = ExtrudeSide::Outward;
    const std::optional<FieldVolume> out = brush::mask_extrude(source, m, s);
    s.side = ExtrudeSide::Inward;
    const std::optional<FieldVolume> in = brush::mask_extrude(source, m, s);
    s.side = ExtrudeSide::Centred;
    const std::optional<FieldVolume> mid = brush::mask_extrude(source, m, s);
    REQUIRE(out.has_value());
    REQUIRE(in.has_value());
    REQUIRE(mid.has_value());

    // Outward lies above the surface, inward below it, centred straddles it.
    CHECK(out->eval(cf3(0, kRadius + 0.05f, 0)) < 0.0f);
    CHECK(out->eval(cf3(0, kRadius - 0.05f, 0)) > 0.0f);

    CHECK(in->eval(cf3(0, kRadius - 0.05f, 0)) < 0.0f);
    CHECK(in->eval(cf3(0, kRadius + 0.05f, 0)) > 0.0f);

    CHECK(mid->eval(cf3(0, kRadius - 0.03f, 0)) < 0.0f);
    CHECK(mid->eval(cf3(0, kRadius + 0.03f, 0)) < 0.0f);
}

TEST_CASE("mask extrude: the rim rounds") {
    const MaskField m = cap_mask();
    MaskExtrudeSettings s = plate_settings(0.12f);
    const std::optional<FieldVolume> hard = brush::mask_extrude(sphere_field(), m, s);
    s.border_round = 0.06f;
    const std::optional<FieldVolume> soft = brush::mask_extrude(sphere_field(), m, s);
    REQUIRE(hard.has_value());
    REQUIRE(soft.has_value());

    // A rounded intersection can only remove material, never add it, so the
    // rounded plate is nowhere deeper than the hard one — and is strictly
    // shallower somewhere near the rim, which is what "rounded" means.
    bool shallower_somewhere = false;
    for (float a = 0.0f; a < 6.28f; a += 0.2f) {
        const cfloat3 p = cf3(std::cos(a), 0.0f, std::sin(a)) * 0.28f + cf3(0, kRadius + 0.02f, 0);
        const float h = hard->eval(p);
        const float t = soft->eval(p);
        CHECK(t >= h - 0.01f);
        if (t > h + 0.005f) shallower_somewhere = true;
    }
    CHECK(shallower_somewhere);
}

TEST_CASE("mask extrude: refusals produce nothing") {
    const auto source = sphere_field();
    const MaskExtrudeSettings s = plate_settings();

    // Nothing painted.
    CHECK_FALSE(brush::mask_extrude(source, MaskField(0.03f), s).has_value());

    // Painted, but nowhere near the surface.
    MaskField away(0.03f);
    away.fill(math::Aabb{cf3(4.0f, 4.0f, 4.0f), cf3(4.4f, 4.4f, 4.4f)}, 1.0f);
    CHECK_FALSE(brush::mask_extrude(source, away, s).has_value());

    // A thickness that is not one.
    MaskExtrudeSettings bad = s;
    bad.thickness = 0.0f;
    CHECK_FALSE(brush::mask_extrude(source, cap_mask(), bad).has_value());
    bad.thickness = -0.1f;
    CHECK_FALSE(brush::mask_extrude(source, cap_mask(), bad).has_value());

    // A wall thinner than the cells that would have to hold it.
    bad = s;
    bad.cell_size = 0.05f;
    bad.thickness = 0.02f;
    CHECK_FALSE(brush::mask_extrude(source, cap_mask(), bad).has_value());
}

TEST_CASE("mask extrude: the mask is not consumed") {
    MaskField m = cap_mask();
    const std::size_t before = m.painted_count();
    MaskExtrudeSettings s = plate_settings();
    s.border_smooth = 2;  // the setting most likely to write back
    REQUIRE(brush::mask_extrude(sphere_field(), m, s).has_value());
    CHECK(m.painted_count() == before);
}

TEST_CASE("mask extrude: a ray still lands on it") {
    const std::optional<FieldVolume> plate =
        brush::mask_extrude(sphere_field(), cap_mask(), plate_settings(0.12f));
    REQUIRE(plate.has_value());

    // Sphere trace from outside along -Y, stepping by the declared bound. The
    // two defects add-sampled-fields found were invisible to point probes and
    // only showed up under marching, so this is the check that matters.
    const float lipschitz = std::max(plate->sample_lipschitz(), 1.0f);
    float t = 0.0f;
    bool hit = false;
    for (int i = 0; i < 512 && t < 2.0f; ++i) {
        const float d = plate->eval(cf3(0, 1.2f - t, 0));
        if (d < 1e-3f) {
            hit = true;
            break;
        }
        t += d / lipschitz;
    }
    CHECK(hit);
    CHECK(1.2f - t == doctest::Approx(outer_surface_y(*plate)).epsilon(0.05));
}

// -- the extrude, on voxels ---------------------------------------------------

TEST_CASE("mask extrude: a plate comes off a voxel ball") {
    const VoxelGrid g = ball_grid();
    const MaskField m = cap_mask();
    const MaskExtrudeSettings s = plate_settings(0.12f);

    const std::optional<VoxelGrid> plate = brush::mask_extrude(g, m, s);
    REQUIRE(plate.has_value());
    CHECK(plate->occupied_count() > 0);

    // Roughly thickness / voxel_size cells deep, above the ball's surface.
    const auto lo = plate->bounds_min();
    const auto hi = plate->bounds_max();
    REQUIRE(lo.has_value());
    REQUIRE(hi.has_value());
    const int depth = hi->y - lo->y + 1;
    CHECK(depth >= 3);
    CHECK(depth <= 8);

    // Nothing on the far side.
    CHECK(plate->get({0, static_cast<std::int32_t>(std::floor(-kRadius / g.voxel_size())), 0}) == 0);
}

TEST_CASE("mask extrude: colour comes along, and the source survives") {
    VoxelGrid g = ball_grid();
    const MaskField m = cap_mask();
    const std::size_t before = g.occupied_count();

    const std::optional<VoxelGrid> plate = brush::mask_extrude(g, m, plate_settings());
    REQUIRE(plate.has_value());

    CHECK(g.occupied_count() == before);  // untouched

    // The extract carries the colour the source had, not a default.
    const auto lo = plate->bounds_min();
    REQUIRE(lo.has_value());
    bool found = false;
    for (std::int32_t z = lo->z; z <= plate->bounds_max()->z && !found; ++z)
        for (std::int32_t y = lo->y; y <= plate->bounds_max()->y && !found; ++y)
            for (std::int32_t x = lo->x; x <= plate->bounds_max()->x && !found; ++x) {
                const std::uint8_t idx = plate->get({x, y, z});
                if (idx == 0) continue;
                const cfloat3 c = plate->palette_color(idx);
                CHECK(c.x == doctest::Approx(0.8f));
                CHECK(c.y == doctest::Approx(0.2f));
                found = true;
            }
    CHECK(found);
}

TEST_CASE("mask extrude: the two representations agree") {
    const float vs = 0.03f;
    const VoxelGrid g = ball_grid(vs);
    const MaskField m = cap_mask();
    MaskExtrudeSettings s = plate_settings(0.12f);
    s.cell_size = vs;

    const std::optional<VoxelGrid> voxels = brush::mask_extrude(g, m, s);
    const std::optional<FieldVolume> field_plate = brush::mask_extrude(sphere_field(), m, s);
    REQUIRE(voxels.has_value());
    REQUIRE(field_plate.has_value());

    // Every cell the voxel extract claims must be inside the field extract, or
    // within a voxel of it. "Within a voxel" is the honest tolerance: one is a
    // lattice of cubes and the other an isosurface.
    const auto lo = voxels->bounds_min();
    const auto hi = voxels->bounds_max();
    REQUIRE(lo.has_value());
    std::size_t total = 0, agreeing = 0;
    for (std::int32_t z = lo->z; z <= hi->z; ++z)
        for (std::int32_t y = lo->y; y <= hi->y; ++y)
            for (std::int32_t x = lo->x; x <= hi->x; ++x) {
                if (voxels->get({x, y, z}) == 0) continue;
                const cfloat3 c = cf3(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                      static_cast<float>(z) + 0.5f) *
                                  vs;
                ++total;
                if (field_plate->eval(c) < vs) ++agreeing;
            }
    REQUIRE(total > 0);
    CHECK(static_cast<float>(agreeing) / static_cast<float>(total) > 0.95f);
}

TEST_CASE("mask extrude: voxel refusals produce nothing") {
    const VoxelGrid g = ball_grid();
    CHECK_FALSE(brush::mask_extrude(g, MaskField(0.03f), plate_settings()).has_value());
    CHECK_FALSE(brush::mask_extrude(VoxelGrid(0.03f), cap_mask(), plate_settings()).has_value());

    MaskField away(0.03f);
    away.fill(math::Aabb{cf3(4.0f, 4.0f, 4.0f), cf3(4.4f, 4.4f, 4.4f)}, 1.0f);
    CHECK_FALSE(brush::mask_extrude(g, away, plate_settings()).has_value());

    MaskExtrudeSettings bad = plate_settings();
    bad.thickness = 0.0f;
    CHECK_FALSE(brush::mask_extrude(g, cap_mask(), bad).has_value());
}
