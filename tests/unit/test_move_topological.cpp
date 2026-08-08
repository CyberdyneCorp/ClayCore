// Move Topological (sdf-kernels spec, add-move-topological): a drag weighted by
// distance ALONG THE MATERIAL rather than through space.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
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

math::Aabb region() { return math::Aabb(cf3(-1.1f, -1.0f, -0.6f), cf3(1.1f, 1.1f, 0.6f)); }

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
