// Mask extrude (sdf-kernels and voxel-engine specs, add-mask-extrude): the mask
// measured as a distance, the plate that comes off a surface, and the agreement
// between the two representations that keeps a document meaning one thing.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
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

float outer_surface_radius(const FieldVolume& v, cfloat3 direction) {
    float previous = 1.0f;
    for (float radius = 1.4f; radius > 0.4f; radius -= 0.001f) {
        const float value = v.eval(direction * radius);
        if (value <= 0.0f && previous > 0.0f) return radius;
        previous = value;
    }
    return 0.0f;
}


// A cap whose border is SERRATED: the radius steps between two values with
// angle, so the boundary zig-zags by about two cells. A round border cannot
// show what border_smooth does -- it is already smooth.
MaskField serrated_cap(int teeth, float cell = 0.03f) {
    MaskField m(cell);
    const cfloat3 pole = cf3(0, kRadius, 0);
    const auto to_cell = [cell](float w) { return static_cast<std::int32_t>(std::floor(w / cell)); };
    for (std::int32_t z = to_cell(pole.z - 0.5f); z <= to_cell(pole.z + 0.5f); ++z)
        for (std::int32_t y = to_cell(pole.y - 0.5f); y <= to_cell(pole.y + 0.5f); ++y)
            for (std::int32_t x = to_cell(pole.x - 0.5f); x <= to_cell(pole.x + 0.5f); ++x) {
                const cfloat3 c = cf3(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                      static_cast<float>(z) + 0.5f) * cell;
                const float dx = c.x - pole.x, dy = c.y - pole.y, dz = c.z - pole.z;
                const float ang = std::atan2(dz, dx);
                const float rad = 0.26f + 0.05f * (std::sin(ang * static_cast<float>(teeth)) > 0.0f
                                                       ? 1.0f : 0.0f);
                if (std::sqrt(dx * dx + dz * dz) <= rad && std::fabs(dy) <= 0.25f)
                    m.set({x, y, z}, 1.0f);
            }
    return m;
}

// How RAGGED the plate's rim is: the total variation of the field around a
// circle that crosses the teeth. A serrated rim swings in and out of material
// and accumulates; a smoothed one does not.
double rim_variation(const FieldVolume& v, float radius, float y) {
    const int kSteps = 720;
    double tv = 0.0;
    float prev = 0.0f;
    for (int i = 0; i <= kSteps; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(kSteps);
        const float f = v.eval(cf3(std::cos(a) * radius, y, std::sin(a) * radius));
        if (i) tv += std::fabs(static_cast<double>(f) - static_cast<double>(prev));
        prev = f;
    }
    return tv;
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

TEST_CASE("mask extrude: the requested thickness survives beyond the painted mask") {
    const MaskField mask = cap_mask();
    for (const float thickness : {0.05f, 0.1f, 0.6f}) {
        MaskExtrudeSettings settings = plate_settings(thickness);
        settings.cell_size = thickness < 0.2f ? 0.01f : 0.03f;
        const std::optional<FieldVolume> plate = brush::mask_extrude(sphere_field(), mask, settings);
        REQUIRE(plate.has_value());

        // Three surface normals across the painted patch must reach the same
        // height even when the paint itself stops short of that height.
        std::vector<float> heights;
        for (const float angle : {0.0f, 0.25f, 0.45f}) {
            const cfloat3 direction = cf3(std::sin(angle), std::cos(angle), 0.0f);
            const float height = outer_surface_radius(*plate, direction) - kRadius;
            heights.push_back(height);
            CAPTURE(thickness);
            CAPTURE(angle);
            CAPTURE(height);
            CHECK(height == doctest::Approx(thickness).epsilon(0.1));
        }
        const auto [lowest, highest] = std::minmax_element(heights.begin(), heights.end());
        CHECK(*highest - *lowest <= thickness * 0.1f);
    }
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

TEST_CASE("mask extrude: voxel wall height is not capped by mask depth") {
    const VoxelGrid source = ball_grid();
    const MaskField mask = cap_mask();
    for (const float thickness : {0.05f, 0.1f, 0.6f}) {
        const std::optional<VoxelGrid> plate =
            brush::mask_extrude(source, mask, plate_settings(thickness));
        REQUIRE(plate.has_value());
        const std::optional<VoxelCoord> top = plate->bounds_max();
        REQUIRE(top.has_value());
        const float height = (static_cast<float>(top->y) + 0.5f) * source.voxel_size() - kRadius;
        CAPTURE(thickness);
        CAPTURE(height);
        CHECK(std::fabs(height - thickness) <= source.voxel_size());
    }
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

TEST_CASE("mask extrude: a thick voxel wall tracks the field extract") {
    const float cell = 0.03f;
    const VoxelGrid grid = ball_grid(cell);
    const MaskField mask = cap_mask(cell);
    MaskExtrudeSettings settings = plate_settings(0.6f);
    settings.cell_size = cell;
    const std::optional<VoxelGrid> voxels = brush::mask_extrude(grid, mask, settings);
    const std::optional<FieldVolume> field = brush::mask_extrude(sphere_field(), mask, settings);
    REQUIRE(voxels.has_value());
    REQUIRE(field.has_value());

    const auto lo = voxels->bounds_min();
    const auto hi = voxels->bounds_max();
    REQUIRE(lo.has_value());
    REQUIRE(hi.has_value());
    std::size_t total = 0, agreeing = 0;
    for (std::int32_t z = lo->z; z <= hi->z; ++z)
        for (std::int32_t y = lo->y; y <= hi->y; ++y)
            for (std::int32_t x = lo->x; x <= hi->x; ++x) {
                if (voxels->get({x, y, z}) == 0) continue;
                ++total;
                const cfloat3 point = cf3(static_cast<float>(x) + 0.5f,
                                           static_cast<float>(y) + 0.5f,
                                           static_cast<float>(z) + 0.5f) * cell;
                if (field->eval(point) < cell) ++agreeing;
            }
    REQUIRE(total > 0);
    CAPTURE(total);
    CAPTURE(agreeing);
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

TEST_CASE("mask_to_field: the threshold decides what counts as masked") {
    // A mask painted at partial strength has no boundary until one is chosen,
    // and choosing it is the caller's rather than a constant buried inside.
    MaskField m(0.05f);
    // Full strength toward a partial TARGET, so the painted level really is
    // 0.4: paint moves each cell toward the target BY the brush weight, so a
    // strength of 0.4 toward 0.4 would land at 0.16 instead.
    voxel::BrushParams p;
    p.size = 13;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = voxel::BrushFalloff::Constant;
    p.strength = 1.0f;
    m.paint(cf3(0, 0, 0), p, 0.4f);
    REQUIRE(m.sample(cf3(0, 0, 0)) == doctest::Approx(0.4f).epsilon(0.02));

    const std::optional<FieldVolume> counted = brush::mask_to_field(m, 0.2f, 0.2f, 0.2f);
    REQUIRE(counted.has_value());
    CHECK(counted->eval(cf3(0, 0, 0)) < 0.0f);

    // Above the paint level nothing is masked, so either there is no volume at
    // all or its centre reads as outside — never as inside.
    const std::optional<FieldVolume> ignored = brush::mask_to_field(m, 0.8f, 0.2f, 0.2f);
    if (ignored.has_value()) CHECK(ignored->eval(cf3(0, 0, 0)) > 0.0f);
}

TEST_CASE("mask_to_field: two disjoint painted regions both measure as inside") {
    // A mask is not one blob. The result has to have two zero sets with the gap
    // between them reading as OUTSIDE — which is what an unsigned "distance to
    // the nearest painted cell" would get wrong, and what makes this usable as
    // a gate rather than only as a border preview.
    MaskField m(0.05f);
    voxel::BrushParams p;
    p.size = 9;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = voxel::BrushFalloff::Constant;
    p.strength = 1.0f;
    m.paint(cf3(-0.5f, 0, 0), p, 1.0f);
    m.paint(cf3(0.5f, 0, 0), p, 1.0f);

    const std::optional<FieldVolume> d = brush::mask_to_field(m, 0.5f, 0.2f, 0.2f);
    REQUIRE(d.has_value());
    CHECK(d->eval(cf3(-0.5f, 0, 0)) < 0.0f);
    CHECK(d->eval(cf3(0.5f, 0, 0)) < 0.0f);
    CHECK(d->eval(cf3(0.0f, 0, 0)) > 0.0f);
}

TEST_CASE("mask_to_field: the same mask measures the same way twice") {
    // Determinism across a round trip of the MASK, since the measurement is
    // derived rather than stored: what has to hold is that a reloaded mask
    // measures identically, not that the volume itself was serialized.
    const MaskField m = cap_mask();
    const std::vector<std::uint8_t> bytes = m.serialize();
    const std::optional<MaskField> back = MaskField::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());

    const std::optional<FieldVolume> a = brush::mask_to_field(m, 0.5f, 0.25f, 0.3f);
    const std::optional<FieldVolume> b = brush::mask_to_field(*back, 0.5f, 0.25f, 0.3f);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->sample_count() == b->sample_count());
    for (int i = 0; i < 64; ++i) {
        const cfloat3 q = cf3(static_cast<float>(i % 4 - 2) * 0.2f,
                              kRadius + static_cast<float>((i / 4) % 4 - 2) * 0.15f,
                              static_cast<float>((i / 16) % 4 - 2) * 0.2f);
        REQUIRE(a->eval(q) == b->eval(q));  // exactly: same input, same measurement
    }
}

TEST_CASE("mask extrude: border_smooth rounds the rim it is asked to round") {
    // WHY (issue #596). The two places that set this field assert only that the
    // CALLER'S MASK survives -- `painted_count == before`, here and in
    // bindings/python/tests/test_pyclay.py:2905. The comment there says what it
    // is for: "the setting most likely to write back". The field is a VEHICLE
    // for a non-consumption check, not its subject, so nothing asserted that
    // any smoothing happens.
    //
    // Measured rather than assumed: deleting both `shaped.smooth(...)` calls in
    // src/brush/mask_extrude.cpp fails ZERO of the suite's 16,633,178
    // assertions.
    const auto variation_at = [](int passes) {
        MaskExtrudeSettings s = plate_settings(0.12f);
        s.border_smooth = passes;
        MaskField m = serrated_cap(9);
        const std::optional<FieldVolume> plate = brush::mask_extrude(sphere_field(), m, s);
        REQUIRE(plate.has_value());
        return rim_variation(*plate, 0.28f, kRadius + 0.05f);
    };

    const double ragged = variation_at(0);
    const double smoothed = variation_at(8);
    CAPTURE(ragged);
    CAPTURE(smoothed);

    // The fixture has to BE ragged, or two smooth rims would agree and the
    // comparison below would pass for the wrong reason.
    CHECK(ragged > 0.1);
    CHECK(smoothed < 0.75 * ragged);

    // And it is a dial rather than a switch: more passes never read rougher.
    //
    // The tolerance is not decoration. Measured at this cell size:
    //
    //     0 passes   0.434444031562
    //     1 pass     0.434444074461   <- UP by 4.3e-8
    //     2 passes   0.431344956305
    //     4 passes   0.365491159202
    //     8 passes   0.254901115055
    //
    // A single pass moves the rim by nothing -- one part in ten million -- and
    // the sign of that nothing is arbitrary, because `MaskField::smooth` is a
    // seven-point integer average and one pass flips almost no cell across the
    // 0.5 threshold (4576 cells above it, 4560 after). Demanding a strict
    // decrease from every step would assert the kernel's STRENGTH, which is not
    // what this field promises; the tolerance is set far above that noise and
    // far below the real steps, which are three to four orders larger.
    double last = 1e9;
    for (const int passes : {0, 1, 2, 4, 8}) {
        const double v = variation_at(passes);
        CAPTURE(passes);
        CAPTURE(v);
        CHECK(v <= last * (1.0 + 1e-4));
        last = v;
    }
}

// -- the projection is paid only where the mask can matter --------------------
//
// #667 reads the mask at each sample's projection onto the source, and its
// six-tap gradient cost six more source calls a sample. That took the device
// gate's mask_extrude from 51.6 to 288.3 ms at 10 stamps. The extrude now takes
// the gradient from the lattice it already sampled, and projects only where the
// region can change a stored value. These hold it to that: skipping is
// BIT-IDENTICAL to projecting every sample, the halo stays seamless, and the
// source is called about once a sample. #667's own tests above hold the wall
// to its requested thickness.

namespace {

// The device gate's mask_extrude fixture, on the host: a 0.8 shell, 24 dabs on
// it at the gate's stamp spread, a 0.05 wall at a 0.04 cell.
constexpr float kShellRadius = 0.8f;

cfloat3 stamp_direction(int i) {
    const double m[3] = {0.4142135624, 0.7320508076, 0.2360679775};
    float c[3];
    for (int a = 0; a < 3; ++a) {
        const double frac = std::fmod(static_cast<double>(i) * m[a], 1.0);
        c[a] = static_cast<float>(frac) * 1.6f - 0.8f;
    }
    const cfloat3 p = cf3(c[0], c[1], c[2]);
    return p / std::max(kernel::clength(p), 1e-6f);
}

MaskField dabbed_shell_mask(float cell = 0.04f) {
    MaskField m(cell);
    voxel::BrushParams b;
    b.size = 6;
    b.shape = voxel::BrushShape::Sphere;
    b.falloff = voxel::BrushFalloff::Smooth;
    b.strength = 1.0f;
    b.seed = 1;
    for (int i = 0; i < 24; ++i) m.paint(stamp_direction(i) * kShellRadius, b, 1.0f);
    return m;
}

// A mask along one edge of a box: the source's gradient turns a corner under it.
MaskField box_edge_mask(float half, float cell = 0.02f) {
    MaskField m(cell);
    const auto to_cell = [cell](float w) { return static_cast<std::int32_t>(std::floor(w / cell)); };
    for (std::int32_t z = to_cell(-0.3f); z <= to_cell(0.3f); ++z)
        for (std::int32_t y = to_cell(half - 0.15f); y <= to_cell(half + 0.15f); ++y)
            for (std::int32_t x = to_cell(half - 0.15f); x <= to_cell(half + 0.15f); ++x)
                m.set({x, y, z}, 1.0f);
    return m;
}

auto box_field(float half) {
    return [half](cfloat3 p) {
        const cfloat3 q = cf3(std::abs(p.x) - half, std::abs(p.y) - half, std::abs(p.z) - half);
        const cfloat3 out = cf3(std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f));
        return kernel::clength(out) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    };
}

// Every sample the lattice visits, stored or not: bricks times samples a brick.
double lattice_samples(const FieldVolume& v) {
    double bricks = 1.0;
    for (int a = 0; a < 3; ++a)
        bricks *= static_cast<double>((v.sample_extent(a) - 1) / field::kBrickDim);
    return bricks * static_cast<double>(field::kBrickSamples);
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

struct Case {
    const char* name;
    std::function<float(cfloat3)> source;
    MaskField mask;
    MaskExtrudeSettings settings;
};

MaskExtrudeSettings with(float thickness, ExtrudeSide side, float round, float cell = 0.0f) {
    MaskExtrudeSettings s;
    s.thickness = thickness;
    s.side = side;
    s.border_round = round;
    s.cell_size = cell;
    return s;
}

std::vector<Case> identity_cases() {
    std::vector<Case> cases;
    const MaskField cap = cap_mask();
    for (const ExtrudeSide side : {ExtrudeSide::Outward, ExtrudeSide::Inward, ExtrudeSide::Centred})
        for (const float round : {0.0f, 0.06f})
            cases.push_back({"sphere cap", sphere_field(), cap, with(0.12f, side, round)});
    // #660's walls: the requested thickness beyond the painted mask.
    cases.push_back({"#660 0.05", sphere_field(), cap, with(0.05f, ExtrudeSide::Outward, 0.0f, 0.01f)});
    cases.push_back({"#660 0.1", sphere_field(), cap, with(0.1f, ExtrudeSide::Outward, 0.0f, 0.01f)});
    cases.push_back({"#660 0.6", sphere_field(), cap, with(0.6f, ExtrudeSide::Outward, 0.0f, 0.03f)});
    cases.push_back({"#660 0.6 rounded inward", sphere_field(), cap,
                     with(0.6f, ExtrudeSide::Inward, 0.05f, 0.03f)});
    for (const ExtrudeSide side : {ExtrudeSide::Outward, ExtrudeSide::Centred})
        for (const float round : {0.0f, 0.04f})
            cases.push_back({"box edge", box_field(0.4f), box_edge_mask(0.4f),
                             with(0.08f, side, round, 0.02f)});
    MaskExtrudeSettings smoothed = with(0.09f, ExtrudeSide::Outward, 0.03f, 0.03f);
    smoothed.border_smooth = 2;
    cases.push_back({"serrated, smoothed", sphere_field(), serrated_cap(9), smoothed});
    cases.push_back({"device shell", sphere_field(kShellRadius), dabbed_shell_mask(),
                     with(0.05f, ExtrudeSide::Outward, 0.0f, 0.04f)});
    cases.push_back({"device shell rounded inward", sphere_field(kShellRadius),
                     dabbed_shell_mask(), with(0.05f, ExtrudeSide::Inward, 0.02f, 0.04f)});
    return cases;
}

}  // namespace

TEST_CASE("mask extrude: skipping the projection changes no stored bit") {
    for (const Case& c : identity_cases()) {
        CAPTURE(c.name);
        CAPTURE(static_cast<int>(c.settings.side));
        CAPTURE(c.settings.border_round);
        const std::optional<FieldVolume> fast = brush::mask_extrude(c.source, c.mask, c.settings);
        const std::optional<FieldVolume> reference =
            brush::detail::mask_extrude_unculled(c.source, c.mask, c.settings);
        REQUIRE(fast.has_value() == reference.has_value());
        if (!fast) continue;
        // The whole serialized volume: lattice, index, far bounds, every stored
        // sample and the measured Lipschitz, compared as bits.
        CHECK(fast->brick_count() > 0);
        CHECK(same_bits(fast->to_blob(), reference->to_blob()));
    }
}

TEST_CASE("mask extrude: neighbouring bricks agree on every sample they share") {
    // The projection's gradient is a lattice difference, and a sample on a brick
    // face takes its outside neighbour from the source rather than from the
    // brick. Both bricks must still store the same bits there, or the halo that
    // makes a brick self-contained becomes a seam.
    for (const Case& c : identity_cases()) {
        CAPTURE(c.name);
        const std::optional<FieldVolume> v = brush::mask_extrude(c.source, c.mask, c.settings);
        REQUIRE(v.has_value());
        const std::vector<float> blob = v->to_blob();
        const int count[3] = {static_cast<int>(blob[5]), static_cast<int>(blob[6]),
                              static_cast<int>(blob[7])};
        const auto index_base = static_cast<std::size_t>(blob[8]);
        const auto data_base = static_cast<std::size_t>(blob[10]);
        const auto offset = [&](int x, int y, int z) {
            return static_cast<std::int32_t>(
                blob[index_base + static_cast<std::size_t>((z * count[1] + y) * count[0] + x)]);
        };
        constexpr int n = field::kBrickDim + 1;
        const auto sample = [&](std::int32_t brick, int x, int y, int z) {
            return blob[data_base + static_cast<std::size_t>(brick) +
                        static_cast<std::size_t>((z * n + y) * n + x)];
        };
        std::size_t faces = 0, mismatched = 0;
        for (int z = 0; z < count[2]; ++z)
            for (int y = 0; y < count[1]; ++y)
                for (int x = 0; x < count[0]; ++x) {
                    const std::int32_t here = offset(x, y, z);
                    if (here < 0) continue;
                    for (int axis = 0; axis < 3; ++axis) {
                        const int next[3] = {x + (axis == 0), y + (axis == 1), z + (axis == 2)};
                        if (next[axis] >= count[axis]) continue;
                        const std::int32_t there = offset(next[0], next[1], next[2]);
                        if (there < 0) continue;
                        ++faces;
                        for (int a = 0; a < n; ++a)
                            for (int b = 0; b < n; ++b) {
                                // `here`'s far face against `there`'s near one.
                                int p[3], q[3];
                                p[axis] = field::kBrickDim;
                                q[axis] = 0;
                                p[(axis + 1) % 3] = q[(axis + 1) % 3] = a;
                                p[(axis + 2) % 3] = q[(axis + 2) % 3] = b;
                                const float u = sample(here, p[0], p[1], p[2]);
                                const float w = sample(there, q[0], q[1], q[2]);
                                if (std::memcmp(&u, &w, sizeof(float)) != 0) ++mismatched;
                            }
                    }
                }
        CHECK(faces > 0);
        CHECK(mismatched == 0);
    }
}

TEST_CASE("mask extrude: the source is called about once a sample") {
    // Counted, not timed. Per lattice sample rather than per stored one, so
    // sparsity cannot flatter it: #667's formula was exactly 7 here (the
    // distance and a six-tap gradient), and the extrude before it exactly 1.
    const MaskField mask = dabbed_shell_mask();
    const auto shell = sphere_field(kShellRadius);
    std::size_t calls = 0;
    const std::function<float(cfloat3)> counted = [&](cfloat3 p) {
        ++calls;
        return shell(p);
    };
    const std::optional<FieldVolume> v =
        brush::mask_extrude(counted, mask, with(0.05f, ExtrudeSide::Outward, 0.0f, 0.04f));
    REQUIRE(v.has_value());
    const double per_sample = static_cast<double>(calls) / lattice_samples(*v);
    CAPTURE(calls);
    CAPTURE(per_sample);
    MESSAGE("source calls per lattice sample: " << per_sample);
    CHECK(per_sample >= 1.0);
    CHECK(per_sample <= 1.5);
}

