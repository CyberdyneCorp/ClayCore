// Relaxing a sampled field (sdf-kernels spec, add-sdf-relax).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/kernel/exactness.h"
#include "clay/scene/tape.h"

using namespace clay;
using field::FieldVolume;
using field::RelaxSettings;
using kernel::cf3;

namespace {

// A sphere with a ripple on it: something with a feature small enough for
// smoothing to remove, on a shape large enough to survive.
auto bumpy_field(float r, float amplitude, float frequency) {
    return [r, amplitude, frequency](kernel::cfloat3 p) {
        float len = kernel::clength(p);
        if (len < 1e-5f) return -r;
        float bump = amplitude * std::sin(frequency * p.x) * std::sin(frequency * p.y) *
                     std::sin(frequency * p.z);
        return len - r + bump;
    };
}

FieldVolume bumpy_volume(float r = 0.7f, float amplitude = 0.05f, float frequency = 14.0f,
                         float cell = 0.03f) {
    return FieldVolume::sample(bumpy_field(r, amplitude, frequency),
                               math::Aabb{cf3(-1.1f, -1.1f, -1.1f), cf3(1.1f, 1.1f, 1.1f)}, cell,
                               0.12f);
}

// How far the surface wanders from a perfect sphere: the thing relaxing should
// reduce. Measured where the volume actually stores samples.
float ripple_amplitude(const FieldVolume& v, float r) {
    float worst = 0.0f;
    for (float a = 0.0f; a < 6.28f; a += 0.07f)
        for (float b = 0.3f; b < 2.9f; b += 0.11f) {
            kernel::cfloat3 dir = cf3(std::sin(b) * std::cos(a), std::cos(b), std::sin(b) * std::sin(a));
            kernel::cfloat3 p = dir * r;
            if (!v.has_samples_at(p)) continue;
            worst = std::max(worst, std::abs(v.eval(p)));
        }
    return worst;
}

// The steepest slope anywhere the volume stores samples: the Lipschitz bound
// the whole design rests on.
float steepest_slope(const FieldVolume& v) {
    const float h = 1e-3f;
    float worst = 0.0f;
    auto sampled = [&](kernel::cfloat3 p) { return v.has_samples_at(p); };
    for (float x = -1.0f; x <= 1.0f; x += 0.023f)
        for (float y = -1.0f; y <= 1.0f; y += 0.029f) {
            kernel::cfloat3 p = cf3(x, y, 0.011f);
            if (!sampled(p) || !sampled(cf3(x + h, y, 0.011f)) || !sampled(cf3(x - h, y, 0.011f)) ||
                !sampled(cf3(x, y + h, 0.011f)) || !sampled(cf3(x, y - h, 0.011f)))
                continue;
            float gx = (v.eval(cf3(x + h, y, 0.011f)) - v.eval(cf3(x - h, y, 0.011f))) / (2 * h);
            float gy = (v.eval(cf3(x, y + h, 0.011f)) - v.eval(cf3(x, y - h, 0.011f))) / (2 * h);
            worst = std::max(worst, std::sqrt(gx * gx + gy * gy));
        }
    return worst;
}

// Roughly how much space the shape encloses, by counting negative samples.
int enclosed(const FieldVolume& v) {
    int count = 0;
    for (float x = -1.05f; x <= 1.05f; x += 0.05f)
        for (float y = -1.05f; y <= 1.05f; y += 0.05f)
            for (float z = -1.05f; z <= 1.05f; z += 0.05f)
                if (v.eval(cf3(x, y, z)) < 0.0f) ++count;
    return count;
}

scene::Tape compile_volume(const FieldVolume& v) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<FieldVolume>(v);
    l.sdf->insert(std::move(n));
    return scene::compile_document(doc);
}

}  // namespace

TEST_CASE("relax: a bumpy surface gets smoother, and more passes smooth more") {
    FieldVolume rough = bumpy_volume();
    float before = ripple_amplitude(rough, 0.7f);

    RelaxSettings once;
    once.radius_cells = 2;
    FieldVolume smoothed = field::relax(rough, once);
    float after = ripple_amplitude(smoothed, 0.7f);

    RelaxSettings thrice = once;
    thrice.iterations = 3;
    float after_three = ripple_amplitude(field::relax(rough, thrice), 0.7f);

    INFO("ripple: " << before << " -> " << after << " -> " << after_three);
    CHECK(after < before * 0.9f);
    CHECK(after_three < after);
}

TEST_CASE("relax: a smooth surface stays where it was") {
    // Smoothing shrinks convex features, so a sphere does move — but by the
    // sampling, not by a visible amount.
    const float r = 0.7f;
    FieldVolume sphere = FieldVolume::sample(
        [r](kernel::cfloat3 p) { return kernel::clength(p) - r; },
        math::Aabb{cf3(-1.1f, -1.1f, -1.1f), cf3(1.1f, 1.1f, 1.1f)}, 0.03f, 0.12f);

    RelaxSettings settings;
    settings.radius_cells = 2;
    FieldVolume smoothed = field::relax(sphere, settings);

    float worst = 0.0f;
    for (float x = -0.8f; x <= 0.8f; x += 0.037f)
        for (float y = -0.8f; y <= 0.8f; y += 0.041f) {
            kernel::cfloat3 p = cf3(x, y, 0.013f);
            if (!smoothed.has_samples_at(p) || !sphere.has_samples_at(p)) continue;
            worst = std::max(worst, std::abs(smoothed.eval(p) - sphere.eval(p)));
        }
    INFO("worst movement on a sphere: " << worst);
    CHECK(worst < 0.03f);  // a cell's worth, not a feature's worth
}

TEST_CASE("relax: the slope does not grow") {
    // The whole design rests on this. Averaging cannot make a field vary
    // faster than it already did, and a field whose slope is bounded by one is
    // automatically a conservative bound on the distance to its own zero set —
    // so sphere tracing on a relaxed field cannot overstep.
    FieldVolume rough = bumpy_volume();
    RelaxSettings settings;
    settings.radius_cells = 2;
    settings.iterations = 2;
    FieldVolume smoothed = field::relax(rough, settings);

    float before = steepest_slope(rough);
    float after = steepest_slope(smoothed);
    INFO("steepest slope: " << before << " -> " << after);
    CHECK(after <= before + 1e-2f);
    // ...and still inside what the tape declares for a sampled volume.
    CHECK(after <= std::sqrt(3.0f) + 1e-2f);
}

TEST_CASE("relax: a ray still finds the surface") {
    FieldVolume smoothed = field::relax(bumpy_volume(), RelaxSettings{1.0f, 2, 2, cf3(0, 0, 0),
                                                                     0.0f, 0.0f, {}});
    scene::Tape tape = compile_volume(smoothed);
    const float scale = kernel::csafe_step_scale(tape.info);

    float t = 0.0f;
    bool hit = false;
    for (int i = 0; i < 500; ++i) {
        float d = tape.eval(cf3(0, 0, 3.0f - t)).d;
        if (d < 1e-4f) {
            hit = true;
            break;
        }
        t += d * scale;
        if (t > 6.0f) break;
    }
    REQUIRE(hit);
    // The relaxed sphere is near radius 0.7, so the hit is near z = 0.7.
    INFO("hit at t = " << t);
    CHECK(t > 1.9f);
    CHECK(t < 2.5f);
}

TEST_CASE("relax: outside the region nothing moves") {
    FieldVolume rough = bumpy_volume();
    RelaxSettings settings;
    settings.radius_cells = 2;
    settings.centre = cf3(0.7f, 0, 0);  // a patch on the +X side
    settings.region_radius = 0.3f;
    settings.falloff = 0.15f;
    FieldVolume patched = field::relax(rough, settings);

    // Far from the region, on the other side of the shape, the field is as it
    // was — to within re-sampling, which every relax pays.
    float worst_far = 0.0f;
    for (float y = -0.8f; y <= 0.8f; y += 0.05f) {
        kernel::cfloat3 p = cf3(-0.7f, y, 0.02f);
        if (!rough.has_samples_at(p)) continue;
        worst_far = std::max(worst_far, std::abs(patched.eval(p) - rough.eval(p)));
    }
    INFO("worst change on the far side: " << worst_far);
    CHECK(worst_far < 0.01f);

    SUBCASE("and inside it something does") {
        // Measured against the far side rather than against a chosen number:
        // the claim is that relax acts here and not there, and the ripple's
        // own amplitude varies over the surface, so an absolute threshold
        // would be a statement about where the probe landed.
        float moved = 0.0f;
        for (float a = -0.3f; a <= 0.3f; a += 0.02f) {
            kernel::cfloat3 p = cf3(0.7f * std::cos(a), 0.7f * std::sin(a), 0.02f);
            if (!rough.has_samples_at(p)) continue;
            moved = std::max(moved, std::abs(patched.eval(p) - rough.eval(p)));
        }
        INFO("largest change inside the region: " << moved << ", outside: " << worst_far);
        CHECK(moved > 0.0f);
        CHECK(moved > worst_far * 100.0f);
    }

    SUBCASE("and the region's edge does not leave a rim") {
        // Walked ALONG the surface, not into it. A radial walk would leave the
        // band and measure the volume's own seam between sampled and bounded
        // space, which has nothing to do with the region.
        //
        // What must be smooth is how much relax CHANGED things: it goes from
        // full effect at the centre to none past the falloff, and if that
        // transition stepped, the surface would carry a visible ridge.
        float previous_delta = 0.0f;
        bool have_previous = false;
        float biggest_jump = 0.0f;
        for (float a = 0.0f; a <= 1.2f; a += 0.01f) {
            kernel::cfloat3 p = cf3(0.7f * std::cos(a), 0.7f * std::sin(a), 0.02f);
            if (!rough.has_samples_at(p) || !patched.has_samples_at(p)) continue;
            float delta = patched.eval(p) - rough.eval(p);
            if (have_previous) biggest_jump = std::max(biggest_jump, std::abs(delta - previous_delta));
            previous_delta = delta;
            have_previous = true;
        }
        INFO("largest step in the change, crossing the region edge: " << biggest_jump);
        CHECK(have_previous);
        CHECK(biggest_jump < 0.01f);
    }
}

TEST_CASE("relax: strength scales the effect") {
    FieldVolume rough = bumpy_volume();
    float before = ripple_amplitude(rough, 0.7f);
    float previous = before;
    for (float strength : {0.25f, 0.5f, 1.0f}) {
        RelaxSettings settings;
        settings.radius_cells = 2;
        settings.strength = strength;
        float got = ripple_amplitude(field::relax(rough, settings), 0.7f);
        CAPTURE(strength);
        CHECK(got <= previous + 1e-3f);
        previous = got;
    }
    CHECK(previous < before);

    SUBCASE("and zero strength changes nothing") {
        RelaxSettings none;
        none.strength = 0.0f;
        FieldVolume same = field::relax(rough, none);
        for (float x = -0.9f; x <= 0.9f; x += 0.07f) {
            kernel::cfloat3 p = cf3(x, 0.1f, 0.02f);
            if (!rough.has_samples_at(p)) continue;
            CHECK(same.eval(p) == doctest::Approx(rough.eval(p)).epsilon(0.02).scale(0.01));
        }
    }
}

TEST_CASE("relax: repeated relaxing settles rather than running away") {
    // Smoothing shrinks convex features. Run enough passes and a naive scheme
    // either eats the shape or inflates it; this must do neither.
    FieldVolume v = bumpy_volume(0.7f, 0.05f, 14.0f, 0.05f);
    int start = enclosed(v);
    RelaxSettings settings;
    settings.radius_cells = 1;
    settings.iterations = 8;
    int after = enclosed(field::relax(v, settings));

    INFO("enclosed cells: " << start << " -> " << after);
    CHECK(after > start / 2);       // it has not been eaten
    CHECK(after < start * 2);       // nor has it run away
}

TEST_CASE("relax: an empty volume is handed back unchanged") {
    FieldVolume nothing;
    CHECK(field::relax(nothing).empty());
}

TEST_CASE("relax: the result is an ordinary item") {
    FieldVolume smoothed = field::relax(bumpy_volume());
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node box;
    box.prim = scene::Prim::box(cf3(1.0f, 1.0f, 1.0f));
    l.sdf->insert(std::move(box));
    scene::Node cut;
    cut.prim = scene::Prim::volume();
    cut.volume = std::make_shared<FieldVolume>(smoothed);
    cut.op = scene::Op::Subtract;
    l.sdf->insert(std::move(cut));

    scene::Tape tape = scene::compile_document(doc);
    CHECK(tape.eval(cf3(0, 0, 0)).d > 0.0f);
    CHECK(tape.eval(cf3(0.95f, 0.95f, 0.95f)).d < 0.0f);
    CHECK_FALSE(tape.info.is_exact);
}

TEST_CASE("a region-limited relax touches nothing outside its taper") {
    // What makes a dab cost what it moves: relax rewrites only the bricks its
    // region meets. The field beyond the taper must be the input's, sample for
    // sample — not close to it, equal to it — because relax returns `here`
    // there and always did, and the region limit is only allowed to skip work
    // that would have been the identity.
    const math::Aabb whole{cf3(-1, -1, -1), cf3(1, 1, 1)};
    const float cell = 0.02f;
    auto bumpy = [](kernel::cfloat3 p) {
        return kernel::clength(p) -
               (0.7f + 0.05f * std::sin(11.0f * p.x) * std::sin(11.0f * p.y) *
                           std::sin(11.0f * p.z));
    };
    const FieldVolume base = FieldVolume::sample(bumpy, whole, cell, cell * 3.0f);

    field::RelaxSettings settings;
    settings.strength = 0.6f;
    settings.radius_cells = 2;
    settings.iterations = 3;  // more than one pass: the region must hold across all of them
    settings.centre = cf3(0.7f, 0, 0);
    settings.region_radius = 0.15f;
    settings.falloff = 0.05f;
    const FieldVolume out = field::relax(base, settings);

    // The taper relax actually uses, not the one asked for: a falloff narrower
    // than the kernel is silently widened, so the untouched set is measured
    // against the widened one.
    const float taper = std::max(settings.falloff,
                                 cell * static_cast<float>(settings.radius_cells) * 2.0f);
    const float reach = settings.region_radius + taper;

    int compared = 0, changed_inside = 0;
    for (int gz = 0; gz < base.sample_extent(2); ++gz)
        for (int gy = 0; gy < base.sample_extent(1); ++gy)
            for (int gx = 0; gx < base.sample_extent(0); ++gx) {
                const std::optional<float> a = base.sample_at(gx, gy, gz);
                const std::optional<float> b = out.sample_at(gx, gy, gz);
                REQUIRE(a.has_value() == b.has_value());
                if (!a) continue;
                if (kernel::clength(base.cell_position(gx, gy, gz) - settings.centre) > reach) {
                    ++compared;
                    REQUIRE(*a == *b);
                } else if (*a != *b) {
                    ++changed_inside;
                }
            }
    CHECK(compared > 0);
    // And it did DO something, so the case above is not passing because relax
    // became a no-op.
    CHECK(changed_inside > 0);
}
