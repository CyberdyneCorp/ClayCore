// Move Topological (sdf-kernels spec, add-move-topological): a drag weighted by
// distance ALONG THE MATERIAL rather than through space.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/field/move_topological.h"
#include "clay/scene/tape.h"

using namespace clay;
using field::FieldVolume;
using field::TopologicalMoveSettings;
using kernel::cf3;

namespace {

// Two prongs, close in space and joined only through a bar below them: the case
// Euclidean distance cannot tell apart from one solid lump.
float two_prongs(kernel::cfloat3 p) {
    auto bar = [](kernel::cfloat3 q) {
        kernel::cfloat3 d = cf3(std::abs(q.x) - 0.35f, std::abs(q.y + 0.42f) - 0.10f,
                                std::abs(q.z) - 0.12f);
        return std::min(std::max(d.x, std::max(d.y, d.z)), 0.0f) +
               kernel::clength(cf3(std::max(d.x, 0.0f), std::max(d.y, 0.0f),
                                   std::max(d.z, 0.0f)));
    };
    auto prong = [](kernel::cfloat3 q, float x) {
        const float dy = std::max(std::abs(q.y - 0.10f) - 0.45f, 0.0f);
        return kernel::clength(cf3(q.x - x, dy, q.z)) - 0.11f;
    };
    return std::min(bar(p), std::min(prong(p, -0.26f), prong(p, 0.26f)));
}

math::Aabb region() { return math::Aabb{cf3(-1.1f, -1.0f, -0.6f), cf3(1.1f, 1.1f, 0.6f)}; }

// Where the surface sits along X at a height, scanning from one side.
float edge_at(const FieldVolume& v, float y, float from, float to) {
    const float step = (to - from) / 900.0f;
    for (int i = 0; i <= 900; ++i) {
        const float x = from + step * static_cast<float>(i);
        if (v.eval(cf3(x, y, 0)) <= 0.0f) return x;
    }
    return 99.0f;
}

FieldVolume moved(float radius, kernel::cfloat3 displacement) {
    TopologicalMoveSettings s;
    s.anchor = cf3(-0.26f, 0.45f, 0);
    s.radius = radius;
    s.displacement = displacement;
    return field::move_topological(two_prongs, region(), 0.02f, 0.07f, s);
}

}  // namespace

TEST_CASE("move topological: the neighbouring prong is not dragged") {
    // The whole brush. Along the material the far prong is about 1.5 away —
    // down one prong, across the bar and up the other — while through space it
    // is 0.32. A radius of 0.5 spans the gap and must still not reach it.
    FieldVolume before = FieldVolume::sample(two_prongs, region(), 0.02f, 0.07f);
    FieldVolume after = moved(0.5f, cf3(-0.25f, 0, 0));

    const float right_before = edge_at(before, 0.45f, 0.0f, 0.6f);
    const float right_after = edge_at(after, 0.45f, 0.0f, 0.6f);
    INFO("the far prong's near edge: " << right_before << " -> " << right_after);
    CHECK(right_after == doctest::Approx(right_before).epsilon(0.0).scale(1.0).epsilon(0.03));

    SUBCASE("while the prong under the anchor does move") {
        const float left_before = edge_at(before, 0.45f, -0.9f, 0.0f);
        const float left_after = edge_at(after, 0.45f, -0.9f, 0.0f);
        INFO("the grabbed prong's outer edge: " << left_before << " -> " << left_after);
        CHECK(left_after < left_before - 0.1f);
    }
}

TEST_CASE("move topological: distance runs along the material, so reach can find it") {
    // Raise the radius past the path through the bar and the far prong comes
    // into range — which is what proves the weight is a distance and not a mask.
    FieldVolume before = FieldVolume::sample(two_prongs, region(), 0.02f, 0.07f);
    const float right_before = edge_at(before, 0.45f, 0.0f, 0.6f);

    const float near_reach = edge_at(moved(0.5f, cf3(-0.2f, 0, 0)), 0.45f, 0.0f, 0.6f);
    const float far_reach = edge_at(moved(2.2f, cf3(-0.2f, 0, 0)), 0.45f, 0.0f, 0.6f);
    INFO("far prong at radius 0.5: " << near_reach << ", at 2.2: " << far_reach);
    CHECK(near_reach == doctest::Approx(right_before).epsilon(0.0).scale(1.0).epsilon(0.03));
    CHECK(far_reach < right_before - 0.02f);
}

TEST_CASE("move topological: the grabbed part keeps its shape") {
    // It translates rather than shearing: the shell carries the geodesic value
    // of the nearest material, so the weight is constant across it.
    FieldVolume before = FieldVolume::sample(two_prongs, region(), 0.02f, 0.07f);
    FieldVolume after = moved(0.5f, cf3(-0.25f, 0, 0));
    auto width = [](const FieldVolume& v) {
        return edge_at(v, 0.45f, 0.0f, -0.9f) - edge_at(v, 0.45f, -0.9f, 0.0f);
    };
    INFO("grabbed prong width " << width(before) << " -> " << width(after));
    CHECK(width(after) == doctest::Approx(width(before)).epsilon(0.25));
}

TEST_CASE("move topological: a drag that reaches nothing changes nothing") {
    FieldVolume before = FieldVolume::sample(two_prongs, region(), 0.02f, 0.07f);

    TopologicalMoveSettings away;
    away.anchor = cf3(0, 5.0f, 0);  // nowhere near material
    away.radius = 0.4f;
    away.displacement = cf3(0.2f, 0, 0);
    FieldVolume untouched = field::move_topological(two_prongs, region(), 0.02f, 0.07f, away);

    TopologicalMoveSettings still;
    still.anchor = cf3(-0.26f, 0.45f, 0);
    still.radius = 0.4f;
    still.displacement = cf3(0, 0, 0);  // no drag at all
    FieldVolume nudged = field::move_topological(two_prongs, region(), 0.02f, 0.07f, still);

    for (float x = -0.8f; x <= 0.8f; x += 0.043f)
        for (float y = -0.6f; y <= 0.9f; y += 0.051f) {
            const kernel::cfloat3 p = cf3(x, y, 0.01f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(untouched.eval(p) == doctest::Approx(before.eval(p)));
            CHECK(nudged.eval(p) == doctest::Approx(before.eval(p)));
        }
}

TEST_CASE("move topological: it declares the steepness it measured") {
    FieldVolume after = moved(0.5f, cf3(-0.25f, 0, 0));
    // A weight that varies along the surface can steepen the field; the point
    // is that the number is read back rather than assumed.
    CHECK(after.sample_lipschitz() > 0.0f);
    CHECK(after.sample_lipschitz() == doctest::Approx(after.measure_sample_lipschitz()));
}

TEST_CASE("move topological: the source volume's feather survives the rebuild") {
    // This verb is the only volume operation that REBUILDS its result: `relax`
    // starts from a copy and `flatten` edits in place, so both carry the
    // feather without trying, while this one re-samples through a callable that
    // knows nothing about the volume it came from. Reported by a host whose
    // user saw a hard box of visible lattice around a move that had otherwise
    // worked -- the displacement was correct throughout.
    FieldVolume before = FieldVolume::sample(two_prongs, region(), 0.02f, 0.07f);
    before.set_feather(0.05f);
    REQUIRE(before.feather() == doctest::Approx(0.05f));

    TopologicalMoveSettings s;
    s.anchor = cf3(-0.26f, 0.45f, 0);
    s.radius = 0.3f;
    s.displacement = cf3(0, 0.12f, 0);
    CHECK(field::move_topological(before, s).feather() == doctest::Approx(before.feather()));

    // The drag-that-touches-nothing path re-samples too rather than handing
    // back the source, so it drops the feather by the same mechanism.
    TopologicalMoveSettings still = s;
    still.displacement = cf3(0, 0, 0);
    CHECK(field::move_topological(before, still).feather() == doctest::Approx(before.feather()));
}

namespace {

float unit_sphere(kernel::cfloat3 p) { return kernel::clength(p) - 1.0f; }

// The probe from issue #657: a unit sphere's top, baked with a band wide
// enough to cover the drag, so the volume itself is never what runs out.
FieldVolume sphere_cap() {
    const math::Aabb box{cf3(-0.4f, -0.4f, 0.5f), cf3(0.9f, 0.4f, 1.8f)};
    return FieldVolume::sample(unit_sphere, box, 0.01f, 0.67f);
}

TopologicalMoveSettings long_drag() {
    TopologicalMoveSettings s;
    s.anchor = cf3(0, 0, 1);
    s.radius = 0.3f;
    s.displacement = cf3(0.5f, 0, 0.4f);  // |d| = 0.64, past twice the reach
    s.ease = 0;
    return s;
}

// The surface height straight down at (x, 0), scanning from the top.
float height_at(const FieldVolume& v, float x) {
    for (int i = 0; i <= 1300; ++i) {
        const float z = 1.8f - 0.001f * static_cast<float>(i);
        if (v.eval(cf3(x, 0, z)) <= 0.0f) return z;
    }
    return -99.0f;
}

}  // namespace

TEST_CASE("move topological: a drag longer than the reach does not fold (#657)") {
    // The single-step pull-back p - d*w(g(p)) stops being one-to-one once
    // |d| * slope / radius passes one: two output points read the same source
    // point, the anchor sinks below the surface it was grabbed from and the
    // pulled material falls away before the end of the drag. Measured at
    // v0.120.1 the anchor sat at 0.940 against an original 1.000.
    //
    // The reference is the host's workaround: the same drag as nine calls of
    // d/9, each anchored where the previous one left the grip. Heights along
    // x = 0 .. 0.35 from the issue. The engine takes five slices here rather
    // than nine, which reads about 0.03 higher along the whole drag; the
    // single step it replaced was 0.16 low at the anchor and 0.40 low at 0.35.
    const float reference[] = {1.097f, 1.129f, 1.162f, 1.196f, 1.232f, 1.267f, 1.302f, 1.335f};
    const FieldVolume after = field::move_topological(sphere_cap(), long_drag());

    float previous = -1.0f;
    for (int i = 0; i < 8; ++i) {
        const float x = 0.05f * static_cast<float>(i);
        const float h = height_at(after, x);
        CAPTURE(x);
        INFO("height " << h << " against the stepped reference " << reference[i]);
        CHECK(h >= 1.0f);     // never below the surface it was grabbed from
        CHECK(h > previous);  // rises along the drag: no crater, no lump
        CHECK(std::abs(h - reference[i]) <= 0.04f);
        previous = h;
    }
}

TEST_CASE("move topological: the sub-steps are what n host calls would give") {
    // The engine's slices compose their pull-backs and read the source once;
    // a host gets the same drag by calling n times and re-sampling the volume
    // n times. Same slices, same anchors, same geodesics -- so the same shape,
    // up to the re-sampling the host pays for and the engine does not.
    const TopologicalMoveSettings drag = long_drag();
    const int n = field::topological_move_steps(drag, 0.01f);
    REQUIRE(n > 1);

    FieldVolume host = sphere_cap();
    const kernel::cfloat3 slice = drag.displacement * (1.0f / static_cast<float>(n));
    for (int i = 0; i < n; ++i) {
        TopologicalMoveSettings step = drag;
        step.anchor = drag.anchor + slice * static_cast<float>(i);
        step.displacement = slice;
        host = field::move_topological(host, step);
    }
    const FieldVolume engine = field::move_topological(sphere_cap(), drag);

    for (int i = 0; i < 8; ++i) {
        const float x = 0.05f * static_cast<float>(i);
        CAPTURE(x);
        CHECK(std::abs(height_at(engine, x) - height_at(host, x)) <= 0.01f);
    }
}

TEST_CASE("move topological: how many slices a drag takes") {
    // Every count below is at the probe's cell size, 0.01: the slope that
    // decides it is the curve's steepest secant over one cell of the reach.
    TopologicalMoveSettings s = long_drag();
    // |d| = 0.64 against half of 0.3 on a linear curve: 4.27, so five.
    CHECK(field::topological_move_steps(s, 0.01f) == 5);

    // A steeper curve needs more slices for the same drag: smoothstep peaks at
    // 1.5, and over one cell its secant is within 0.001 of that, so 6.4 ->
    // seven. A smooth curve loses nothing to the secant; a cusped one does.
    s.ease = kernel::ease_smoothstep;
    CHECK(field::topological_move_steps(s, 0.01f) == 7);

    // Under half the radius on a linear curve is one step -- the single
    // pull-back unchanged, which is every drag the older tests here make.
    s.ease = 0;
    s.displacement = cf3(0.15f, 0, 0);
    CHECK(field::topological_move_steps(s, 0.01f) == 1);
    s.radius = 0.5f;
    s.displacement = cf3(-0.25f, 0, 0);
    CHECK(field::topological_move_steps(s, 0.01f) == 1);

    // Capped, so one call cannot be asked for unbounded work.
    s.radius = 0.01f;
    s.displacement = cf3(100.0f, 0, 0);
    CHECK(field::topological_move_steps(s, 0.01f) == 64);

    // Nothing to move: one (empty) step rather than a division by zero.
    s.radius = 0.0f;
    CHECK(field::topological_move_steps(s, 0.01f) == 1);
}

TEST_CASE("move topological: a short drag on a circ curve is a few slices, not the cap") {
    // The circ family's analytic slope is 71.7, reached only in a band of t
    // 1e-4 wide at CLAY_CIRC_GUARD -- about 3e-5 of distance at radius 0.3, far
    // under one cell. Sized off that supremum, a drag of half the radius went
    // to the 64-slice cap, and since each slice reads its grid through every
    // slice before it, the call went from about 95 ms to 13 s. The slope the
    // lattice can see is the steepest secant over one cell: about 7.7 here.
    TopologicalMoveSettings s;
    s.anchor = cf3(0, 0, 1);
    s.radius = 0.3f;
    const std::uint8_t circs[] = {kernel::ease_in_circ, kernel::ease_out_circ,
                                  kernel::ease_in_out_circ};
    for (const std::uint8_t ease : circs) {
        CAPTURE(int(ease));
        s.ease = ease;
        s.displacement = cf3(0.15f, 0, 0);  // half the radius: was 64
        CHECK(field::topological_move_steps(s, 0.01f) == 8);
        s.displacement = cf3(0.05f, 0, 0);  // was 24
        CHECK(field::topological_move_steps(s, 0.01f) == 3);
        s.displacement = cf3(0.01f, 0, 0);  // was 5
        CHECK(field::topological_move_steps(s, 0.01f) == 1);
        // With no cell to resolve against, the supremum is all there is.
        s.displacement = cf3(0.15f, 0, 0);
        CHECK(field::topological_move_steps(s, 0.0f) == 64);
    }

    // The secant never exceeds the supremum, so a linear drag sitting exactly
    // on the bound is not pushed over a ceil() by rounding.
    s.ease = kernel::ease_linear;
    s.displacement = cf3(0.15f, 0, 0);
    CHECK(field::topological_move_steps(s, 0.01f) == 1);
    CHECK(field::topological_move_steps(s, 0.001f) == 1);
}

TEST_CASE("move topological: a short circ drag costs about what a linear one does") {
    // The runtime half of the case above, as a RATIO against the same drag on
    // a linear curve in the same build, so it holds under a sanitizer or a
    // debug build as well as in release. Measured in release on the volume
    // below: linear 93 ms; in_circ 151 ms at 3 slices, against 2,053 ms at
    // the 24 the supremum asked for -- 1.6x where it was 22x.
    const math::Aabb box{cf3(-0.4f, -0.4f, 0.5f), cf3(0.9f, 0.4f, 1.8f)};
    const FieldVolume cap = FieldVolume::sample(unit_sphere, box, 0.01f, 0.2f);
    TopologicalMoveSettings s;
    s.anchor = cf3(0, 0, 1);
    s.radius = 0.3f;
    s.displacement = cf3(0.05f, 0, 0);

    auto fastest_ms = [&cap](const TopologicalMoveSettings& drag) {
        double best = 1e30;
        for (int run = 0; run < 2; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            const FieldVolume out = field::move_topological(cap, drag);
            const auto t1 = std::chrono::steady_clock::now();
            CHECK(!out.empty());
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    s.ease = kernel::ease_linear;
    const double linear = fastest_ms(s);
    s.ease = kernel::ease_in_circ;
    REQUIRE(field::topological_move_steps(s, 0.01f) == 3);
    const double circ = fastest_ms(s);
    INFO("linear " << linear << " ms, in_circ " << circ << " ms");
    CHECK(circ < 6.0 * linear);
}
