// Flattening a sampled field onto a plane (sdf-kernels spec, add-sdf-flatten).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
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
    return FieldVolume::sample(f, math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)},
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

const math::Aabb kRegion{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)};
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
        // version to negotiate — it simply has no such field. The current
        // header is 14 floats ([12] the feather, [13] the colour offset); a
        // pre-Lipschitz layout drops all three appended fields.
        std::vector<float> older = blob;
        REQUIRE(static_cast<std::size_t>(older[8]) == 14);
        older.erase(older.begin() + 11, older.begin() + 14);
        older[8] = 11;
        older[9] = older[9] - 3;
        older[10] = older[10] - 3;
        auto legacy = FieldVolume::from_blob(older);
        REQUIRE(legacy.has_value());
        CHECK(legacy->sample_lipschitz() == doctest::Approx(1.0f));
        CHECK(legacy->brick_count() == after.brick_count());
    }

    SUBCASE("and a blob written before COLOUR existed reads as uncoloured") {
        // The same rule one field later. A 13-float header has no slot 13, and
        // reading one there would read the first index entry as an offset —
        // which is why from_blob asks the header size rather than the blob
        // length.
        std::vector<float> older = blob;
        REQUIRE(static_cast<std::size_t>(older[8]) == 14);
        older.erase(older.begin() + 13);
        older[8] = 13;
        older[9] = older[9] - 1;
        older[10] = older[10] - 1;
        auto legacy = FieldVolume::from_blob(older);
        REQUIRE(legacy.has_value());
        CHECK_FALSE(legacy->has_color());
        CHECK(legacy->brick_count() == after.brick_count());
        CHECK(legacy->sample_lipschitz() == doctest::Approx(after.sample_lipschitz()));
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

// -- flattening a volume in place (make-the-flatten-dab-local) ---------------

namespace {

// A ball at the origin, plus `extra` unrelated ones marching off in +x that no
// brush here ever reaches. What "unrelated model" means for a scaling test.
FieldVolume balls_volume(int extra, float cell) {
    auto f = [extra](kernel::cfloat3 p) {
        float d = kernel::clength(p) - 0.6f;
        for (int i = 1; i <= extra; ++i)
            d = std::min(d, kernel::clength(p - cf3(1.6f * static_cast<float>(i), 0, 0)) - 0.6f);
        return d;
    };
    return FieldVolume::sample(
        f, math::Aabb{cf3(-1, -1, -1), cf3(1.0f + 1.6f * static_cast<float>(extra), 1, 1)}, cell,
        cell * 4.0f);
}

// How many stored samples differ between two volumes over the same lattice,
// and how many of the ones that survived are the identical float.
struct SampleDiff {
    int changed = 0;
    int identical = 0;
    int appeared = 0;
    int vanished = 0;
};

SampleDiff diff_samples(const FieldVolume& a, const FieldVolume& b, int extent) {
    SampleDiff d;
    for (int gx = 0; gx < extent; ++gx)
        for (int gy = 0; gy < extent; ++gy)
            for (int gz = 0; gz < extent; ++gz) {
                const std::optional<float> was = a.sample_at(gx, gy, gz);
                const std::optional<float> now = b.sample_at(gx, gy, gz);
                if (!was && !now) continue;
                if (!was) {
                    ++d.appeared;
                    continue;
                }
                if (!now) {
                    ++d.vanished;
                    continue;
                }
                if (*was == *now)
                    ++d.identical;
                else
                    ++d.changed;
            }
    return d;
}

// A brush on the north pole of the ball at the origin, with a plane cutting
// through it.
FlattenSettings pole_brush(float cell, kernel::cfloat3 centre = cf3(0, 0.6f, 0)) {
    FlattenSettings s;
    s.plane_point = cf3(0, 0.55f, 0);
    s.plane_normal = cf3(0, 1, 0);
    s.centre = centre;
    s.region_radius = 5.0f * cell;
    s.falloff = 5.0f * cell;
    return s;
}

}  // namespace

TEST_CASE("flatten on a volume touches only the bricks its brush reaches") {
    // The locality claim, read back through the stored samples rather than
    // through eval. Every sample beyond the brush and its taper must be the
    // float it was — not close to it, the same one — because the fill hands
    // back the volume's own stored sample wherever the weight is zero.
    const float cell = 0.03f;
    const FieldVolume before = balls_volume(0, cell);
    const FlattenSettings settings = pole_brush(cell);
    const FieldVolume after = field::flatten(before, settings);
    REQUIRE_FALSE(after.empty());

    const float reach = settings.region_radius + settings.falloff;
    int compared = 0, moved = 0, dropped = 0;
    for (int gx = 0; gx < 80; ++gx)
        for (int gy = 0; gy < 80; ++gy)
            for (int gz = 0; gz < 80; ++gz) {
                const std::optional<float> was = before.sample_at(gx, gy, gz);
                if (!was) continue;
                const kernel::cfloat3 p = before.cell_position(gx, gy, gz);
                const float d = kernel::clength(p - settings.centre);
                const std::optional<float> now = after.sample_at(gx, gy, gz);
                if (d > reach) {
                    // Outside the taper the VALUE cannot move. The brick may
                    // still stop storing it -- a brick straddling the taper
                    // whose in-brush samples all left the band is emptied as a
                    // whole -- and then no copy of it survives, which the loop
                    // below requires to have been beyond the band anyway.
                    if (now) {
                        REQUIRE(*now == *was);
                        ++compared;
                    } else {
                        REQUIRE(std::abs(*was) > before.band());
                        ++dropped;
                    }
                } else if (now && *now != *was) {
                    ++moved;
                }
            }
    CHECK(compared > 1000);
    CHECK(moved > 0);  // the brush did something, or the above is vacuous
    INFO("dropped " << dropped);
}

TEST_CASE("flatten's cost follows the brush, not the model around it") {
    // The scaling law, as counts rather than a stopwatch. The same dab into a
    // volume covering four times the surface must change the same samples and
    // add the same bricks; before this it re-sampled the whole bounds and the
    // second volume came back with everything rewritten.
    const float cell = 0.03f;
    SampleDiff first;
    std::ptrdiff_t first_bricks = 0;
    for (int extra : {0, 3}) {
        CAPTURE(extra);
        const FieldVolume before = balls_volume(extra, cell);
        const FieldVolume after = field::flatten(before, pole_brush(cell));
        REQUIRE_FALSE(after.empty());
        // Only the lattice around the first ball; the others sit beyond it.
        const SampleDiff d = diff_samples(before, after, 70);
        const std::ptrdiff_t bricks = static_cast<std::ptrdiff_t>(after.brick_count()) -
                                      static_cast<std::ptrdiff_t>(before.brick_count());
        if (extra == 0) {
            first = d;
            first_bricks = bricks;
            REQUIRE(d.changed > 0);
        } else {
            CHECK(d.changed == first.changed);
            CHECK(d.appeared == first.appeared);
            CHECK(d.vanished == first.vanished);
            CHECK(bricks == first_bricks);
        }
    }
}

TEST_CASE("flatten on a volume does not re-record its far bounds as samples") {
    // Sampling a volume through eval() reads a BOUND outside the band — one
    // that steps by brick rather than by cell — and storing those steps as
    // samples inflated a re-baked ball from 444 stored bricks to 1,246 and
    // declared a Lipschitz of 14 where its source declared 1. A local flatten
    // reads the stored sample instead, so only the brush can move either
    // number.
    const float cell = 0.03f;
    const FieldVolume before = balls_volume(0, cell);
    REQUIRE(before.sample_lipschitz() == doctest::Approx(1.0f).epsilon(0.01));

    const FieldVolume after = field::flatten(before, pole_brush(cell));
    CHECK(after.sample_lipschitz() < 4.0f);
    // A five-cell dab on a ball of hundreds of bricks cannot double it.
    CHECK(static_cast<double>(after.brick_count()) <
          1.15 * static_cast<double>(before.brick_count()));

    SUBCASE("and the inflation does not compound over a stroke") {
        FieldVolume rolling = before;
        for (int dab = 0; dab < 4; ++dab) rolling = field::flatten(rolling, pole_brush(cell));
        CHECK(rolling.sample_lipschitz() < 4.0f);
        CHECK(static_cast<double>(rolling.brick_count()) <
              1.15 * static_cast<double>(before.brick_count()));
    }
}

TEST_CASE("flatten on a volume puts the facet where the plane is") {
    // The blend still means what it meant. Compared against the same flatten
    // sampled from the EXACT field the volume was built from, which is the
    // oracle — not the old whole-bounds implementation, whose answer this
    // change is correcting.
    const float cell = 0.02f;
    auto exact = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.6f; };
    const math::Aabb box{cf3(-1, -1, -1), cf3(1, 1, 1)};
    const FieldVolume before = FieldVolume::sample(exact, box, cell, cell * 4.0f);

    for (FlattenMode mode : {FlattenMode::TwoSided, FlattenMode::CutOnly, FlattenMode::FillOnly}) {
        CAPTURE(static_cast<int>(mode));
        FlattenSettings settings = pole_brush(cell);
        settings.plane_point = cf3(0, 0.52f, 0);
        settings.region_radius = 0.12f;
        settings.falloff = 0.08f;
        settings.mode = mode;

        const FieldVolume local = field::flatten(before, settings);
        const FieldVolume oracle = field::flatten(std::function<float(kernel::cfloat3)>(exact), box,
                                                  cell, cell * 4.0f, settings);
        REQUIRE_FALSE(local.empty());

        // Under the brush, on the plane. CutOnly removes the cap, FillOnly has
        // nothing below the plane to fill and so leaves the ball alone.
        const float expected = mode == FlattenMode::FillOnly ? 0.6f : 0.52f;
        for (float x = -0.06f; x <= 0.06f; x += 0.03f) {
            CAPTURE(x);
            const float got = surface_height(local, x, 0.0f);
            REQUIRE(got > -5.0f);
            CHECK(got == doctest::Approx(expected).epsilon(0.0).scale(1.0).epsilon(0.035));
            CHECK(got == doctest::Approx(surface_height(oracle, x, 0.0f))
                             .epsilon(0.0)
                             .scale(1.0)
                             .epsilon(0.035));
        }
    }
}

TEST_CASE("flatten on a volume can put the facet in bricks that stored nothing") {
    // The reason rewrite_region could not do this job. The plane sits well
    // outside the band around the source surface, so the bricks the facet
    // lands in held no samples at all before the dab.
    const float cell = 0.02f;
    const FieldVolume before =
        FieldVolume::sample([](kernel::cfloat3 p) { return kernel::clength(p) - 0.6f; },
                            math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)}, cell, cell * 4.0f);

    FlattenSettings settings;
    settings.plane_point = cf3(0, 0.42f, 0);
    settings.plane_normal = cf3(0, 1, 0);
    settings.centre = cf3(0, 0.6f, 0);
    settings.region_radius = 0.25f;
    settings.falloff = 0.1f;

    // 0.18 is well past a band of four cells: nothing describes the facet yet.
    REQUIRE_FALSE(before.has_samples_at(cf3(0, 0.42f, 0)));

    const FieldVolume after = field::flatten(before, settings);
    CHECK(after.has_samples_at(cf3(0, 0.42f, 0)));
    CHECK(surface_height(after, 0.0f, 0.0f) ==
          doctest::Approx(0.42f).epsilon(0.0).scale(1.0).epsilon(0.03));
    // And the bricks the surface left no longer claim to hold it.
    CHECK(after.eval(cf3(0, 0.6f, 0)) > 0.0f);
}

TEST_CASE("flatten on a volume keeps the copies of a shared sample together") {
    // A sample on a brick face, edge or corner is stored by every brick
    // sharing it, and a region selects whole bricks — so a brush placed on one
    // of those boundaries has selected and unselected bricks holding the same
    // sample. sample_at answers from whichever brick it finds first, so the
    // copies drifting apart would show as a step in the field rather than as a
    // failure here; serialize() compares the raw arrays and does catch it.
    const float cell = 0.025f;
    const FieldVolume before = balls_volume(0, cell);
    const float brick = static_cast<float>(field::kBrickDim) * cell;
    const kernel::cfloat3 o = before.origin();

    // Snap to a brick corner near the north pole, then step off it by a face,
    // an edge and a corner offset.
    auto on_lattice = [&](float v, float from) {
        return from + std::round((v - from) / brick) * brick;
    };
    const kernel::cfloat3 corner =
        cf3(on_lattice(0.0f, o.x), on_lattice(0.58f, o.y), on_lattice(0.0f, o.z));
    const kernel::cfloat3 places[] = {
        corner + cf3(brick * 0.5f, brick * 0.5f, brick * 0.5f),  // inside a brick
        corner + cf3(0.0f, brick * 0.5f, brick * 0.5f),          // on a face
        corner + cf3(0.0f, 0.0f, brick * 0.5f),                  // on an edge
        corner,                                                  // on a corner
    };

    int which = 0;
    for (const kernel::cfloat3& centre : places) {
        CAPTURE(++which);
        FlattenSettings settings = pole_brush(cell, centre);
        settings.plane_point = cf3(0, 0.5f, 0);
        const FieldVolume after = field::flatten(before, settings);
        REQUIRE_FALSE(after.empty());

        // Every copy of every stored sample agrees. A volume whose halo copies
        // had drifted would still answer sample_at, so this reads the bricks
        // themselves: re-sampling the result at its own sample positions must
        // reproduce the stored value, which is only true when the brick the
        // lookup lands in holds the same float as its neighbours.
        FieldVolume rebuilt = after;
        rebuilt.rewrite([&after](int gx, int gy, int gz, float old) {
            const std::optional<float> s = after.sample_at(gx, gy, gz);
            return s ? *s : old;
        });
        CHECK(rebuilt.serialize() == after.serialize());
    }
}

TEST_CASE("flatten on a volume hands back settings that describe no flatten") {
    // Not a resampled copy of the volume, which is what this used to return
    // and which is neither free nor faithful. The volume IS the source,
    // sampled.
    const float cell = 0.03f;
    const FieldVolume before = balls_volume(0, cell);
    const std::vector<std::uint8_t> was = before.serialize();

    SUBCASE("a zero normal") {
        FlattenSettings s = pole_brush(cell);
        s.plane_normal = cf3(0, 0, 0);
        CHECK(field::flatten(before, s).serialize() == was);
    }
    SUBCASE("no strength") {
        FlattenSettings s = pole_brush(cell);
        s.strength = 0.0f;
        CHECK(field::flatten(before, s).serialize() == was);
    }
    SUBCASE("no region") {
        FlattenSettings s = pole_brush(cell);
        s.region_radius = 0.0f;
        CHECK(field::flatten(before, s).serialize() == was);
    }
    SUBCASE("a region frozen solid by a mask") {
        // The weight is zero at every sample, so the fill is the identity
        // everywhere and the resample writes back what it read.
        FlattenSettings s = pole_brush(cell);
        s.mask = [](kernel::cfloat3) { return 1.0f; };
        CHECK(field::flatten(before, s).serialize() == was);
    }
}

TEST_CASE("flatten declares the Lipschitz its sampling already measured") {
    // sample_blocks ends by measuring what it stored, and the flatten
    // overloads that sample used to measure it a second time — a sweep of
    // every stored sample in the volume for a number already in hand.
    FlattenSettings settings = horizontal_at(0.55f);
    settings.falloff = 0.04f;  // a tight taper, so there is something to declare
    const FieldVolume sampled = flatten_source(settings);
    REQUIRE(sampled.sample_lipschitz() > 1.0f);
    CHECK(sampled.sample_lipschitz() == doctest::Approx(sampled.measure_sample_lipschitz()));
}
