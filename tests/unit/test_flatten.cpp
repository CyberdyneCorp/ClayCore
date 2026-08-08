// Flattening a sampled field onto a plane (sdf-kernels spec, add-sdf-flatten).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "clay/field/flatten.h"
#include "clay/field/volume.h"
#include "clay/kernel/exactness.h"
#include "clay/scene/tape.h"

using namespace clay;
using field::FieldVolume;
using field::FlattenMode;
using field::FlattenSettings;
using kernel::cf3;

namespace {

// A ball with a bump on top and a dent in its side: one feature above the
// plane for flatten to take off, one below it for flatten to fill.
FieldVolume bumpy_ball(float cell = 0.03f) {
    auto f = [](kernel::cfloat3 p) {
        float ball = kernel::clength(p) - 0.6f;
        float bump = kernel::clength(p - cf3(0, 0.62f, 0)) - 0.22f;
        float dent = kernel::clength(p - cf3(0.55f, 0.1f, 0)) - 0.2f;
        return std::max(std::min(ball, bump), -dent);  // union the bump, subtract the dent
    };
    return FieldVolume::sample(f, math::Aabb(cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)),
                               cell, 0.12f);
}

// The bumpy ball as a FORMULA, which is what flatten should sample from: a
// document's tape is exact everywhere, where a volume reports a bound rather
// than a distance outside its band.
std::function<float(kernel::cfloat3)> bumpy_source() {
    return [](kernel::cfloat3 p) {
        float ball = kernel::clength(p) - 0.6f;
        float bump = kernel::clength(p - cf3(0, 0.62f, 0)) - 0.22f;
        float dent = kernel::clength(p - cf3(0.55f, 0.1f, 0)) - 0.2f;
        return std::max(std::min(ball, bump), -dent);
    };
}

const math::Aabb kRegion(cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f));
constexpr float kCell = 0.03f;
constexpr float kBand = 0.12f;

FieldVolume flatten_source(const FlattenSettings& s) {
    return field::flatten(bumpy_source(), kRegion, kCell, kBand, s);
}

// A region is required — flatten is local, and without one it replaces the
// shape with a half-space instead of flattening it. This one covers the bump
// and tapers off well before the rest of the ball.
FlattenSettings horizontal_at(float height) {
    FlattenSettings s;
    s.plane_point = cf3(0, height, 0);
    s.plane_normal = cf3(0, 1, 0);
    s.centre = cf3(0, 0.62f, 0);
    s.region_radius = 0.3f;
    s.falloff = 0.25f;
    return s;
}

// The steepest slope where the volume stores samples. The whole stencil must
// land in sampled space: one straddling the boundary with the bounded space
// beyond measures that seam, not the interpolant.
float steepest_slope(const FieldVolume& v) {
    const float h = 1e-3f;
    float worst = 0.0f;
    auto ok = [&](kernel::cfloat3 p) { return v.has_samples_at(p); };
    for (float x = -1.0f; x <= 1.0f; x += 0.019f)
        for (float y = -1.0f; y <= 1.0f; y += 0.023f) {
            kernel::cfloat3 p = cf3(x, y, 0.011f);
            if (!ok(p) || !ok(cf3(x + h, y, 0.011f)) || !ok(cf3(x - h, y, 0.011f)) ||
                !ok(cf3(x, y + h, 0.011f)) || !ok(cf3(x, y - h, 0.011f)))
                continue;
            float gx = (v.eval(cf3(x + h, y, 0.011f)) - v.eval(cf3(x - h, y, 0.011f))) / (2 * h);
            float gy = (v.eval(cf3(x, y + h, 0.011f)) - v.eval(cf3(x, y - h, 0.011f))) / (2 * h);
            worst = std::max(worst, std::sqrt(gx * gx + gy * gy));
        }
    return worst;
}

// Where the surface sits above (x, z), by walking down until the field turns
// negative. Returns -10 if there is nothing there.
float surface_height(const FieldVolume& v, float x, float z) {
    for (float y = 1.15f; y > -1.15f; y -= 0.004f)
        if (v.eval(cf3(x, y, z)) < 0.0f) return y;
    return -10.0f;
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

TEST_CASE("flatten: a bump becomes a facet on the plane") {
    const float height = 0.55f;
    FieldVolume before = bumpy_ball();
    REQUIRE(surface_height(before, 0, 0) > height + 0.1f);  // the bump stands proud

    FieldVolume after = flatten_source(horizontal_at(height));
    for (float x = -0.15f; x <= 0.15f; x += 0.03f) {
        CAPTURE(x);
        float got = surface_height(after, x, 0.0f);
        REQUIRE(got > -5.0f);
        CHECK(got == doctest::Approx(height).epsilon(0.0).scale(1.0).epsilon(0.04));
    }
}

TEST_CASE("flatten: the band brackets the FLATTENED surface, not the original") {
    // The reason this samples rather than rewriting a volume's samples. The
    // surface moves many band widths, and a band cannot follow: rewriting left
    // the surface outside the samples that described it, and the isosurface
    // came apart. Sampling builds the band around where the surface ended up.
    const float height = 0.55f;
    FieldVolume after = flatten_source(horizontal_at(height));
    // The facet is stored, which is the whole claim.
    CHECK(after.has_samples_at(cf3(0, height, 0)));
    CHECK(after.has_samples_at(cf3(0.1f, height, 0.1f)));
    // ...and where the bump used to be is now empty space, far enough from the
    // new surface that it holds no samples at all.
    CHECK_FALSE(after.has_samples_at(cf3(0, 0.83f, 0)));
    CHECK(after.eval(cf3(0, 0.83f, 0)) > 0.0f);
}

TEST_CASE("flatten: a hollow below the plane is filled, not deepened") {
    // Two-sided, matching sculpt_flatten: material above goes AND hollows below
    // fill. A subtract could only ever do the first.
    FlattenSettings settings;
    settings.plane_point = cf3(0.5f, 0, 0);
    settings.plane_normal = cf3(1, 0, 0);  // flatten the dented side
    settings.centre = cf3(0.5f, 0.1f, 0);
    settings.region_radius = 0.3f;
    settings.falloff = 0.25f;

    FieldVolume before = bumpy_ball();
    const kernel::cfloat3 in_dent = cf3(0.45f, 0.1f, 0);
    REQUIRE(before.eval(in_dent) > 0.0f);  // empty before: it is a hollow

    FieldVolume after = flatten_source(settings);
    INFO("dent reads " << before.eval(in_dent) << " before, " << after.eval(in_dent) << " after");
    CHECK(after.eval(in_dent) < 0.0f);  // filled
}

TEST_CASE("flatten: a surface already on the plane does not move") {
    auto slab = [](kernel::cfloat3 p) {
        return std::max(p.y - 0.3f, kernel::clength(p) - 0.9f);
    };
    FlattenSettings settings = horizontal_at(0.3f);
    settings.centre = cf3(0, 0.3f, 0);
    FieldVolume plain = FieldVolume::sample(slab, kRegion, kCell, kBand);
    FieldVolume after = field::flatten(slab, kRegion, kCell, kBand, settings);

    float worst = 0.0f;
    for (float x = -0.6f; x <= 0.6f; x += 0.037f)
        for (float z = -0.6f; z <= 0.6f; z += 0.041f) {
            kernel::cfloat3 p = cf3(x, 0.3f, z);
            if (!plain.has_samples_at(p) || !after.has_samples_at(p)) continue;
            worst = std::max(worst, std::abs(after.eval(p) - plain.eval(p)));
        }
    INFO("worst movement on a surface already flat: " << worst);
    CHECK(worst < 0.02f);
}

TEST_CASE("flatten: strength scales how far the surface goes") {
    const float height = 0.55f;
    const float start = surface_height(bumpy_ball(), 0, 0);
    float previous = start;
    for (float strength : {0.25f, 0.5f, 1.0f}) {
        FlattenSettings s = horizontal_at(height);
        s.strength = strength;
        float got = surface_height(flatten_source(s), 0, 0);
        CAPTURE(strength);
        CHECK(got <= previous + 1e-3f);  // further with more
        previous = got;
    }
    CHECK(previous == doctest::Approx(height).epsilon(0.0).scale(1.0).epsilon(0.04));

    SUBCASE("and zero strength changes nothing") {
        FlattenSettings none = horizontal_at(height);
        none.strength = 0.0f;
        CHECK(surface_height(flatten_source(none), 0, 0) ==
              doctest::Approx(start).epsilon(0.0).scale(1.0).epsilon(0.02));
    }
}

TEST_CASE("flatten: the declared bound is one the field actually meets") {
    // A brush blends under a weight that varies across its region, and that
    // adds (q - d) grad w — so unlike relax this CAN steepen the field, and the
    // volume has to say so. The bound is MEASURED from the samples rather than
    // guessed in advance.
    FlattenSettings settings = horizontal_at(0.55f);
    settings.centre = cf3(0, 0.6f, 0);
    settings.region_radius = 0.3f;
    settings.falloff = 0.04f;  // deliberately tight

    FieldVolume after = flatten_source(settings);
    CHECK(after.sample_lipschitz() >= 1.0f);

    const float declared = kernel::cfi_volume(after.sample_lipschitz()).lipschitz;
    const float measured = steepest_slope(after);
    INFO("sample lipschitz " << after.sample_lipschitz() << ", declared " << declared
                             << ", measured " << measured);
    CHECK(measured <= declared + 1e-2f);

    SUBCASE("a tighter taper is steeper than a gentle one") {
        FlattenSettings gentle = settings;
        gentle.falloff = 0.35f;
        CHECK(flatten_source(gentle).sample_lipschitz() <= after.sample_lipschitz());
    }

    SUBCASE("a region is required, and without one the shape is left alone") {
        // Flatten is local: where its weight is one the result IS the plane, so
        // with no region at full strength it would replace the ball with a
        // half-space rather than flattening it — a ball comes back as a box.
        FlattenSettings none = horizontal_at(0.55f);
        none.region_radius = 0.0f;
        FieldVolume untouched = flatten_source(none);
        FieldVolume plain = FieldVolume::sample(bumpy_source(), kRegion, kCell, kBand);
        CHECK(surface_height(untouched, 0, 0) ==
              doctest::Approx(surface_height(plain, 0, 0)).epsilon(0.02));
    }

    SUBCASE("and the tape carries it, so the step scale drops accordingly") {
        FlattenSettings gentle = settings;
        gentle.falloff = 0.35f;
        scene::Tape plain = compile_volume(flatten_source(gentle));
        scene::Tape steep = compile_volume(after);
        CHECK(steep.info.lipschitz >= plain.info.lipschitz);
        CHECK(kernel::csafe_step_scale(steep.info) <= kernel::csafe_step_scale(plain.info));
    }
}

TEST_CASE("flatten: a ray still finds the facet") {
    FieldVolume after = flatten_source(horizontal_at(0.55f));
    scene::Tape tape = compile_volume(after);
    const float scale = kernel::csafe_step_scale(tape.info);

    float t = 0.0f;
    bool hit = false;
    for (int i = 0; i < 4000; ++i) {
        float d = tape.eval(cf3(0, 3.0f - t, 0)).d;
        if (d < 1e-4f) {
            hit = true;
            break;
        }
        t += d * scale;
        if (t > 6.0f) break;
    }
    REQUIRE(hit);
    INFO("landed at y = " << 3.0f - t);
    CHECK(3.0f - t == doctest::Approx(0.55f).epsilon(0.0).scale(1.0).epsilon(0.05));
}

TEST_CASE("flatten: outside the region nothing moves, and the edge leaves no rim") {
    FlattenSettings settings = horizontal_at(0.55f);
    settings.centre = cf3(0, 0.6f, 0);
    settings.region_radius = 0.25f;
    settings.falloff = 0.15f;

    FieldVolume plain = FieldVolume::sample(bumpy_source(), kRegion, kCell, kBand);
    FieldVolume after = flatten_source(settings);

    float worst_far = 0.0f;
    for (float x = -0.7f; x <= 0.7f; x += 0.05f) {
        kernel::cfloat3 p = cf3(x, -0.55f, 0.02f);
        if (!plain.has_samples_at(p)) continue;
        worst_far = std::max(worst_far, std::abs(after.eval(p) - plain.eval(p)));
    }
    INFO("worst change on the far side: " << worst_far);
    CHECK(worst_far < 0.01f);

    SUBCASE("and the change tapers rather than stepping") {
        float previous = 0.0f;
        bool have = false;
        float biggest_jump = 0.0f;
        for (float x = 0.0f; x <= 0.9f; x += 0.01f) {
            kernel::cfloat3 p = cf3(x, 0.5f, 0.0f);
            if (!plain.has_samples_at(p) || !after.has_samples_at(p)) continue;
            float delta = after.eval(p) - plain.eval(p);
            if (have) biggest_jump = std::max(biggest_jump, std::abs(delta - previous));
            previous = delta;
            have = true;
        }
        INFO("largest step in the change: " << biggest_jump);
        CHECK(have);
        CHECK(biggest_jump < 0.03f);
    }
}

TEST_CASE("flatten: a degenerate plane is refused rather than guessed at") {
    FlattenSettings bad = horizontal_at(0.5f);
    bad.plane_normal = cf3(0, 0, 0);
    FieldVolume after = flatten_source(bad);
    FieldVolume plain = FieldVolume::sample(bumpy_source(), kRegion, kCell, kBand);
    for (float x = -0.5f; x <= 0.5f; x += 0.07f) {
        kernel::cfloat3 p = cf3(x, 0.2f, 0.02f);
        if (!plain.has_samples_at(p)) continue;
        CHECK(after.eval(p) == doctest::Approx(plain.eval(p)));
    }
}

TEST_CASE("flatten: a volume can be the source, for a shape with no document behind it") {
    // What an imported mesh gives. Accurate while the surface stays near the
    // band it came from, which is what the plane is placed inside here.
    FieldVolume before = bumpy_ball();
    FlattenSettings settings = horizontal_at(0.78f);  // just inside the bump
    FieldVolume after = field::flatten(before, settings);
    REQUIRE_FALSE(after.empty());
    float got = surface_height(after, 0, 0);
    INFO("surface " << surface_height(before, 0, 0) << " -> " << got);
    CHECK(got < surface_height(before, 0, 0));
    CHECK(got == doctest::Approx(0.78f).epsilon(0.0).scale(1.0).epsilon(0.06));
}

TEST_CASE("flatten: an empty volume is handed back unchanged") {
    FieldVolume nothing;
    CHECK(field::flatten(nothing, horizontal_at(0.0f)).empty());
}

TEST_CASE("flatten: the sample Lipschitz survives a round trip") {
    FlattenSettings settings = horizontal_at(0.55f);
    settings.centre = cf3(0, 0.6f, 0);
    settings.region_radius = 0.3f;
    settings.falloff = 0.04f;
    FieldVolume after = flatten_source(settings);
    REQUIRE(after.sample_lipschitz() > 1.0f);  // there is something to preserve

    std::vector<float> blob = after.to_blob();
    auto back = FieldVolume::from_blob(blob);
    REQUIRE(back.has_value());
    CHECK(back->sample_lipschitz() == doctest::Approx(after.sample_lipschitz()));

    SUBCASE("and a blob written before the field existed reads as 1") {
        // The header size IS the index offset, so an older layout is not a
        // version to negotiate — it simply has no such field.
        std::vector<float> older = blob;
        REQUIRE(static_cast<std::size_t>(older[8]) == 12);
        older.erase(older.begin() + 11);
        older[8] = 11;
        older[9] = older[9] - 1;
        older[10] = older[10] - 1;
        auto legacy = FieldVolume::from_blob(older);
        REQUIRE(legacy.has_value());
        CHECK(legacy->sample_lipschitz() == doctest::Approx(1.0f));
        CHECK(legacy->brick_count() == after.brick_count());
    }
}

// -- one-sided modes (hPolish, Planar, the Trim family) -----------------------

TEST_CASE("flatten: cut-only planes the bump and leaves the hollow alone") {
    // The whole difference between Flatten and hPolish. Cutting WITHOUT filling
    // is what leaves a crisp facet against untouched surface; filling the
    // hollows beside a facet is what a polish must not do.
    FlattenSettings settings;
    settings.plane_point = cf3(0.5f, 0, 0);
    settings.plane_normal = cf3(1, 0, 0);
    settings.centre = cf3(0.5f, 0.1f, 0);
    settings.region_radius = 0.3f;
    settings.falloff = 0.25f;

    FieldVolume before = bumpy_ball();
    const kernel::cfloat3 in_dent = cf3(0.45f, 0.1f, 0);
    REQUIRE(before.eval(in_dent) > 0.0f);  // a hollow: empty

    settings.mode = FlattenMode::CutOnly;
    FieldVolume cut = flatten_source(settings);
    INFO("dent reads " << before.eval(in_dent) << " before, " << cut.eval(in_dent)
                       << " after a cut-only flatten");
    CHECK(cut.eval(in_dent) > 0.0f);  // still empty — untouched

    SUBCASE("and it still cuts what stands proud of the plane") {
        FlattenSettings high;
        high.plane_point = cf3(0, 0.55f, 0);
        high.plane_normal = cf3(0, 1, 0);
        high.centre = cf3(0, 0.55f, 0);
        high.region_radius = 0.4f;
        high.falloff = 0.2f;
        high.mode = FlattenMode::CutOnly;
        FieldVolume planed = flatten_source(high);
        CHECK(surface_height(planed, 0, 0) ==
              doctest::Approx(0.55f).epsilon(0.0).scale(1.0).epsilon(0.05));
    }
}

TEST_CASE("flatten: fill-only is the dual — it fills the hollow and spares the bump") {
    FlattenSettings settings;
    settings.plane_point = cf3(0.5f, 0, 0);
    settings.plane_normal = cf3(1, 0, 0);
    settings.centre = cf3(0.5f, 0.1f, 0);
    settings.region_radius = 0.3f;
    settings.falloff = 0.25f;
    settings.mode = FlattenMode::FillOnly;

    const kernel::cfloat3 in_dent = cf3(0.45f, 0.1f, 0);
    CHECK(flatten_source(settings).eval(in_dent) < 0.0f);  // filled

    FlattenSettings high;
    high.plane_point = cf3(0, 0.55f, 0);
    high.plane_normal = cf3(0, 1, 0);
    high.centre = cf3(0, 0.55f, 0);
    high.region_radius = 0.4f;
    high.falloff = 0.2f;
    high.mode = FlattenMode::FillOnly;
    // The bump stands proud of the plane, and fill-only must not touch it.
    CHECK(surface_height(flatten_source(high), 0, 0) ==
          doctest::Approx(surface_height(bumpy_ball(), 0, 0)).epsilon(0.02));
}

TEST_CASE("flatten: asking for no mode is what it always was") {
    // The default has to be the old behaviour, or every existing caller changes
    // meaning under them.
    FlattenSettings settings;
    settings.plane_point = cf3(0, 0.55f, 0);
    settings.plane_normal = cf3(0, 1, 0);
    settings.centre = cf3(0, 0.55f, 0);
    settings.region_radius = 0.4f;
    settings.falloff = 0.2f;
    CHECK(settings.mode == FlattenMode::TwoSided);

    FieldVolume implicit = flatten_source(settings);
    settings.mode = FlattenMode::TwoSided;
    FieldVolume explicit_ = flatten_source(settings);
    for (float x = -0.3f; x <= 0.3f; x += 0.05f)
        for (float y = 0.2f; y <= 0.9f; y += 0.07f) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(implicit.eval(cf3(x, y, 0.02f)) ==
                  doctest::Approx(explicit_.eval(cf3(x, y, 0.02f))));
        }
}

TEST_CASE("flatten: a one-sided result is no steeper than the two-sided one") {
    // The clamp removes movement; it cannot add any. Measured rather than
    // assumed, because the declared Lipschitz is what the marcher trusts.
    FlattenSettings settings;
    settings.plane_point = cf3(0, 0.55f, 0);
    settings.plane_normal = cf3(0, 1, 0);
    settings.centre = cf3(0, 0.55f, 0);
    settings.region_radius = 0.4f;
    settings.falloff = 0.2f;

    FieldVolume two = flatten_source(settings);
    settings.mode = FlattenMode::CutOnly;
    FieldVolume cut = flatten_source(settings);
    settings.mode = FlattenMode::FillOnly;
    FieldVolume fill = flatten_source(settings);

    INFO("lipschitz: two-sided " << two.sample_lipschitz() << ", cut "
                                 << cut.sample_lipschitz() << ", fill "
                                 << fill.sample_lipschitz());
    CHECK(cut.sample_lipschitz() <= two.sample_lipschitz() + 1e-3f);
    CHECK(fill.sample_lipschitz() <= two.sample_lipschitz() + 1e-3f);
}
