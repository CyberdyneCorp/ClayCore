// Magnify and pinch (sdf-kernels + voxel-engine specs, add-magnify-pinch).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/kernel/exactness.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"

using namespace clay;
using kernel::cf3;

namespace {

scene::Document ball_with(const scene::Deformer* d, float r = 0.6f) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    if (d) n.deformers.push_back(*d);
    l.sdf->insert(std::move(n));
    return doc;
}

// Where the surface sits along +X, by walking out until the field turns
// positive.
float surface_x(const scene::Tape& t, float y = 0.0f) {
    for (float x = 0.0f; x < 3.0f; x += 0.002f)
        if (t.eval(cf3(x, y, 0)).d > 0.0f) return x;
    return -1.0f;
}

}  // namespace

TEST_CASE("magnify: a positive strength swells, a negative one gathers") {
    const float r = 0.6f;
    const float plain = surface_x(scene::compile_document(ball_with(nullptr, r)));
    REQUIRE(plain == doctest::Approx(r).epsilon(0.02));

    // About the shape's OWN centre, with the support reaching past its surface.
    // Not about a point on the surface: a radial scale leaves its centre fixed,
    // so a probe running straight through it measures the one place the
    // deformation cannot move — which is what this test first did.
    scene::Deformer swell = scene::Deformer::magnify(cf3(0, 0, 0), 0.9f, 0.5f);
    scene::Deformer gather = scene::Deformer::magnify(cf3(0, 0, 0), 0.9f, -0.5f);

    const float out = surface_x(scene::compile_document(ball_with(&swell, r)));
    const float in = surface_x(scene::compile_document(ball_with(&gather, r)));
    INFO("surface at x: " << in << " (pinched) < " << plain << " (plain) < " << out
                          << " (magnified)");
    CHECK(out > plain);
    CHECK(in < plain);

    SUBCASE("and the centre of the scale is its fixed point") {
        // Worth pinning down, because it is the surprise: a radial scale about
        // a point on the surface bulges the neighbourhood AROUND that point and
        // leaves the point itself exactly where it was.
        scene::Deformer at_surface = scene::Deformer::magnify(cf3(r, 0, 0), 0.45f, 0.5f);
        scene::Tape t = scene::compile_document(ball_with(&at_surface, r));
        CHECK(surface_x(t) == doctest::Approx(plain).epsilon(0.01));
        // ...while off the axis it has moved.
        CHECK(surface_x(t, 0.25f) > surface_x(scene::compile_document(ball_with(nullptr, r)),
                                              0.25f));
    }
}

TEST_CASE("magnify: zero strength changes nothing") {
    scene::Deformer none = scene::Deformer::magnify(cf3(0.6f, 0, 0), 0.45f, 0.0f);
    scene::Tape with = scene::compile_document(ball_with(&none));
    scene::Tape without = scene::compile_document(ball_with(nullptr));
    for (float x = -1.0f; x <= 1.4f; x += 0.037f)
        for (float y = -0.8f; y <= 0.8f; y += 0.041f) {
            kernel::cfloat3 p = cf3(x, y, 0.013f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(with.eval(p).d == doctest::Approx(without.eval(p).d));
        }
}

TEST_CASE("magnify: support really is finite") {
    // Item influence bounds and brick culling both trust this: outside the
    // radius the field must be identical, not merely close.
    const kernel::cfloat3 centre = cf3(0.6f, 0, 0);
    const float radius = 0.35f;
    scene::Deformer d = scene::Deformer::magnify(centre, radius, 0.6f);
    scene::Tape with = scene::compile_document(ball_with(&d));
    scene::Tape without = scene::compile_document(ball_with(nullptr));

    int checked = 0;
    for (float x = -1.4f; x <= 1.8f; x += 0.031f)
        for (float y = -1.2f; y <= 1.2f; y += 0.037f) {
            kernel::cfloat3 p = cf3(x, y, 0.019f);
            if (kernel::clength(p - centre) <= radius) continue;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(with.eval(p).d == doctest::Approx(without.eval(p).d));
            ++checked;
        }
    REQUIRE(checked > 500);  // the partition must not have emptied the test
}

TEST_CASE("magnify: the stretch it costs is declared") {
    // A radial scale is not distance preserving, so the field stops being exact
    // and the tape has to carry the slope — the same treatment grab and pose get.
    scene::Tape plain = scene::compile_document(ball_with(nullptr));
    CHECK(plain.info.is_exact);

    scene::Deformer d = scene::Deformer::magnify(cf3(0.6f, 0, 0), 0.4f, 0.5f);
    scene::Tape warped = scene::compile_document(ball_with(&d));
    CHECK_FALSE(warped.info.is_exact);
    CHECK(warped.info.lipschitz > plain.info.lipschitz);
    CHECK(kernel::csafe_step_scale(warped.info) < kernel::csafe_step_scale(plain.info));

    SUBCASE("and a stronger one declares more") {
        float previous = kernel::csafe_step_scale(plain.info);
        for (float strength : {0.2f, 0.5f, 0.8f}) {
            scene::Deformer s = scene::Deformer::magnify(cf3(0.6f, 0, 0), 0.4f, strength);
            const float scale = kernel::csafe_step_scale(
                scene::compile_document(ball_with(&s)).info);
            CAPTURE(strength);
            CHECK(scale <= previous);
            previous = scale;
        }
    }

    SUBCASE("and pinching costs too, because compressing space is still a stretch") {
        scene::Deformer p = scene::Deformer::magnify(cf3(0.6f, 0, 0), 0.4f, -0.5f);
        scene::Tape pinched = scene::compile_document(ball_with(&p));
        CHECK_FALSE(pinched.info.is_exact);
        CHECK(pinched.info.lipschitz > plain.info.lipschitz);
    }
}

TEST_CASE("magnify: a ray still finds the surface") {
    scene::Deformer d = scene::Deformer::magnify(cf3(0.6f, 0, 0), 0.4f, 0.5f);
    scene::Tape tape = scene::compile_document(ball_with(&d));
    const float scale = kernel::csafe_step_scale(tape.info);

    float t = 0.0f;
    bool hit = false;
    for (int i = 0; i < 20000; ++i) {
        float dist = tape.eval(cf3(3.0f - t, 0, 0)).d;
        if (dist < 1e-4f) {
            hit = true;
            break;
        }
        t += dist * scale;
        if (t > 6.0f) break;
    }
    REQUIRE(hit);
    const float landed = 3.0f - t;
    INFO("landed at x = " << landed);
    // The magnified surface sits beyond the plain sphere's 0.6.
    CHECK(landed > 0.6f);
    CHECK(landed < 1.4f);
}

TEST_CASE("magnify: the bound covers the swelling") {
    // Brick culling trusts the item's bounds, so a magnified item that grew
    // past them would be culled where it is still visible.
    scene::Deformer d = scene::Deformer::magnify(cf3(0.6f, 0, 0), 0.4f, 0.6f);
    scene::Document doc = ball_with(&d);
    scene::Tape tape = scene::compile_document(doc);
    REQUIRE_FALSE(tape.bounds.empty());

    for (float y = -0.6f; y <= 0.6f; y += 0.05f)
        for (float z = -0.6f; z <= 0.6f; z += 0.05f) {
            const float x = surface_x(tape, y);
            if (x < 0.0f) continue;
            CAPTURE(y);
            CHECK(x <= tape.bounds.max.x + 1e-3f);
        }
}

TEST_CASE("magnify: the voxel verb is pinch's inverse") {
    // Two representations sharing a verb's name must share its meaning.
    voxel::VoxelGrid grid(0.05f);
    std::uint8_t colour = grid.palette_add(kernel::cf3(0.7f, 0.5f, 0.3f));
    // A solid ball of cells.
    const int r = 7;
    for (int x = -r; x <= r; ++x)
        for (int y = -r; y <= r; ++y)
            for (int z = -r; z <= r; ++z)
                if (x * x + y * y + z * z <= r * r) grid.set({x, y, z}, colour);
    const std::size_t start = grid.occupied_count();

    // ON the surface. Both verbs move SURFACE cells, so a brush buried in the
    // interior touches nothing — which is what this test first did.
    const voxel::VoxelCoord at{r, 0, 0};
    voxel::BrushParams p;
    p.size = 3;

    // Mean distance of nearby material FROM THE BRUSH CENTRE, which is what the
    // verb is defined against. Not the reach along +X: the cell sitting on the
    // brush centre has no direction to step, so it is the one place neither
    // verb can move — the same fixed point the SDF form has, and the same trap.
    const auto mean_radius = [&](const voxel::VoxelGrid& g) {
        double total = 0.0;
        int n = 0;
        for (int x = at.x - 5; x <= at.x + 5; ++x)
            for (int y = at.y - 5; y <= at.y + 5; ++y)
                for (int z = at.z - 5; z <= at.z + 5; ++z) {
                    if (g.get({x, y, z}) == 0) continue;
                    const double dx = x - at.x, dy = y - at.y, dz = z - at.z;
                    total += std::sqrt(dx * dx + dy * dy + dz * dz);
                    ++n;
                }
        return n ? total / n : 0.0;
    };

    voxel::VoxelGrid pinched = grid;
    pinched.sculpt_pinch(at, p);
    voxel::VoxelGrid magnified = grid;
    magnified.sculpt_magnify(at, p);

    INFO("mean radius from the brush: " << mean_radius(pinched) << " pinched, "
                                        << mean_radius(grid) << " start, "
                                        << mean_radius(magnified) << " magnified");
    CHECK(mean_radius(pinched) < mean_radius(grid));
    CHECK(mean_radius(magnified) > mean_radius(grid));
    (void)start;
}
