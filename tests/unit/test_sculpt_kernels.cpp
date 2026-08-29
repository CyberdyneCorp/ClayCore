// The shared brush kernels, exercised WITHOUT a mesh (add-shared-brush-kernels).
//
// THIS FILE'S INCLUDE LIST IS PART OF THE TEST. The meshing delta requires that
// the kernel interface compile in a translation unit that includes no mesh, no
// adjacency and no BVH header, because that is what "representation-neutral"
// has to mean if the adaptive and multiresolution sculptors are going to call
// it. Adding `clay/mesh/sculpt.h` here to reach a convenience would silently
// retire that guarantee, so it is not here and must not be added.
//
// Everything below builds its snapshot by hand: five entries and their
// one-rings as flat arrays, which is exactly what a sculptor over any
// representation has to produce.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/sculpt_kernels.h"

using namespace clay;
using namespace clay::kernel;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::SculptNeighbors;
using mesh::SculptScratch;
using mesh::SculptSnapshot;

namespace {

// A plus-shaped patch: a centre entry surrounded by four neighbours, all in the
// y = 0 plane except the centre, which is lifted so a smoothing pass has
// something to remove.
struct Patch {
    std::vector<cfloat3> positions{cf3(0, 0.5f, 0), cf3(-1, 0, 0), cf3(1, 0, 0), cf3(0, 0, -1),
                                   cf3(0, 0, 1)};
    std::vector<cfloat3> normals{cf3(0, 1, 0), cf3(0, 1, 0), cf3(0, 1, 0), cf3(0, 1, 0),
                                 cf3(0, 1, 0)};
    std::vector<float> weights{1.0f, 0.5f, 0.5f, 0.5f, 0.5f};

    // Entry 0 rings the other four; each of those rings entry 0 alone.
    std::vector<std::uint32_t> offsets{0, 4, 5, 6, 7, 8};
    std::vector<std::uint32_t> slots{1, 2, 3, 4, 0, 0, 0, 0};
    std::vector<cfloat3> nb_positions{cf3(-1, 0, 0), cf3(1, 0, 0),   cf3(0, 0, -1), cf3(0, 0, 1),
                                      cf3(0, 0.5f, 0), cf3(0, 0.5f, 0), cf3(0, 0.5f, 0),
                                      cf3(0, 0.5f, 0)};

    SculptSnapshot snapshot() const {
        SculptSnapshot s;
        s.positions = positions.data();
        s.normals = normals.data();
        s.weights = weights.data();
        s.count = positions.size();
        s.average_normal = cf3(0, 1, 0);
        s.centroid = cf3(0, 0, 0);
        s.plane_point = cf3(0, 0, 0);
        s.plane_normal = cf3(0, 1, 0);
        return s;
    }

    SculptNeighbors neighbors() const {
        SculptNeighbors n;
        n.offsets = offsets.data();
        n.slots = slots.data();
        n.positions = nb_positions.data();
        return n;
    }
};

MeshBrushSettings base_settings() {
    MeshBrushSettings s;
    s.center = cf3(0, 0, 0);
    s.radius = 1.0f;
    s.strength = 0.5f;
    return s;
}

}  // namespace

TEST_CASE("sculpt kernels: the weight composes in one fixed order") {
    // The identity case has to be EXACT, not close. Every factor a stamp does
    // not use is 1, and a stamp with no gate, no alpha and no automask must
    // reproduce its falloff bit for bit — that property is what let the
    // extraction be compared against main by hash rather than by tolerance.
    mesh::WeightFactors f;
    f.falloff = 0.3f;
    CHECK(mesh::compose_weight(f) == 0.3f);

    // A fully gated vertex weighs exactly zero, so the verb writes nothing and
    // the position it holds is the position it started with.
    f.gate = 1.0f;
    CHECK(mesh::compose_weight(f) == 0.0f);

    // ...and so does a fully automasked one, which is the same requirement
    // reached through the factor this change adds.
    f.gate = 0.0f;
    f.automask = 0.0f;
    CHECK(mesh::compose_weight(f) == 0.0f);

    // The gate's clamp is a domain guard on a caller's callback and is NOT
    // folded into the boundary test: a gate below zero must leave the weight
    // alone rather than amplify it.
    f.automask = 1.0f;
    f.gate = -0.5f;
    CHECK(mesh::compose_weight(f) == 0.3f);
    f.gate = 1.5f;
    CHECK(mesh::compose_weight(f) == 0.0f);
}

TEST_CASE("sculpt kernels: the falloff curves are the voxel curves") {
    // Same four curves, same values, same weights as voxel::falloff_weight —
    // the duplication is the module layering, so the values are what pins them
    // together.
    CHECK(mesh::falloff_weight(mesh::MeshFalloff::Constant, 0.5f) == 1.0f);
    CHECK(mesh::falloff_weight(mesh::MeshFalloff::Linear, 0.25f) == doctest::Approx(0.75f));
    CHECK(mesh::falloff_weight(mesh::MeshFalloff::Smooth, 0.0f) == 1.0f);
    CHECK(mesh::falloff_weight(mesh::MeshFalloff::Smooth, 1.0f) == 0.0f);
    // Clamped at both ends rather than extrapolated.
    CHECK(mesh::falloff_weight(mesh::MeshFalloff::Linear, 2.0f) == 0.0f);
    CHECK(mesh::falloff_weight(mesh::MeshFalloff::Linear, -1.0f) == 1.0f);
}

TEST_CASE("sculpt kernels: the path taper is the identity inside the budget") {
    // A euclidean region passes 1 and pays nothing; a geodesic one inside the
    // taper's start must be untouched, or every ordinary stamp would drift.
    CHECK(mesh::path_taper(0.5f, 1.5f, 2.0f) == 1.0f);
    CHECK(mesh::path_taper(1.5f, 1.5f, 2.0f) == 1.0f);
    CHECK(mesh::path_taper(2.0f, 1.5f, 2.0f) == 0.0f);
    CHECK(mesh::path_taper(3.0f, 1.5f, 2.0f) == 0.0f);  // clamped, not negative
}

TEST_CASE("sculpt kernels: draw and inflate differ only in the frame") {
    Patch patch;
    const SculptSnapshot s = patch.snapshot();
    MeshBrushSettings settings = base_settings();

    std::vector<cfloat3> draw(s.count), inflate(s.count);
    mesh::kernel_draw(s, settings, draw.data());
    mesh::kernel_inflate(s, settings, inflate.data());

    // Every normal in this patch IS the average normal, so the two frames
    // coincide and the two kernels must agree exactly. That is the reading the
    // brush model depends on: draw and inflate are one deformation under two
    // frames, not two deformations.
    for (std::size_t i = 0; i < s.count; ++i) {
        CHECK(draw[i].x == inflate[i].x);
        CHECK(draw[i].y == inflate[i].y);
        CHECK(draw[i].z == inflate[i].z);
    }

    // And the amount is strength * radius, scaled by the weight.
    CHECK(draw[0].y == doctest::Approx(0.5f));
    CHECK(draw[1].y == doctest::Approx(0.25f));

    // Tilt one vertex's own normal and the two must part company, or the frame
    // axis would be decorative.
    patch.normals[1] = cf3(1, 0, 0);
    const SculptSnapshot tilted = patch.snapshot();
    mesh::kernel_inflate(tilted, settings, inflate.data());
    CHECK(inflate[1].x == doctest::Approx(0.25f));
    CHECK(inflate[1].y == doctest::Approx(0.0f));
}

TEST_CASE("sculpt kernels: grab carries the region and nudge slides it") {
    Patch patch;
    const SculptSnapshot s = patch.snapshot();
    MeshBrushSettings settings = base_settings();
    settings.direction = cf3(0.25f, 0.5f, 0.0f);

    std::vector<cfloat3> grab(s.count), nudge(s.count);
    mesh::kernel_grab(s, settings, grab.data());
    mesh::kernel_nudge(s, settings, nudge.data());

    // Grab carries the drag whole, weight-scaled — including the component
    // that leaves the surface.
    CHECK(grab[0].y == doctest::Approx(0.5f));
    // Nudge keeps only what lies IN the surface, which is the whole difference.
    CHECK(nudge[0].y == doctest::Approx(0.0f));
    CHECK(nudge[0].x == doctest::Approx(0.25f));
}

TEST_CASE("sculpt kernels: smoothing reads the ring and a zero weight writes nothing") {
    Patch patch;
    const SculptSnapshot s = patch.snapshot();
    const SculptNeighbors nb = patch.neighbors();
    MeshBrushSettings settings = base_settings();
    settings.strength = 1.0f;
    settings.smooth_iterations = 1;

    SculptScratch scratch;
    std::vector<cfloat3> out(s.count);
    mesh::kernel_smooth_family(MeshBrush::Smooth, s, nb, settings, scratch, out.data());

    // The centre's four neighbours average to y = 0, so a full-weight pass
    // pulls the lifted centre all the way down to them.
    CHECK(out[0].y == doctest::Approx(-0.5f));

    // A zero-weight entry is untouched EXACTLY, not nearly: a rim vertex that
    // drifts by an ulp is a visible seam along every stroke.
    patch.weights[0] = 0.0f;
    const SculptSnapshot zeroed = patch.snapshot();
    mesh::kernel_smooth_family(MeshBrush::Smooth, zeroed, nb, settings, scratch, out.data());
    CHECK(out[0].x == 0.0f);
    CHECK(out[0].y == 0.0f);
    CHECK(out[0].z == 0.0f);
}

TEST_CASE("sculpt kernels: relax slides along the surface where smooth sinks into it") {
    Patch patch;
    const SculptSnapshot s = patch.snapshot();
    const SculptNeighbors nb = patch.neighbors();
    MeshBrushSettings settings = base_settings();
    settings.strength = 1.0f;
    settings.smooth_iterations = 1;

    SculptScratch scratch;
    std::vector<cfloat3> smooth(s.count), relax(s.count);
    mesh::kernel_smooth_family(MeshBrush::Smooth, s, nb, settings, scratch, smooth.data());
    mesh::kernel_relax(s, nb, settings, scratch, relax.data());

    // Same Laplacian target, normal component removed. On this patch the whole
    // target is normal, so relax moves nothing at all while smooth moves half
    // a unit — which is the distinction the two verbs exist for.
    CHECK(smooth[0].y == doctest::Approx(-0.5f));
    CHECK(relax[0].y == doctest::Approx(0.0f));
}

TEST_CASE("sculpt kernels: the colour pair moves no vertex and smear needs a direction") {
    Patch patch;
    const SculptSnapshot s = patch.snapshot();
    const SculptNeighbors nb = patch.neighbors();
    MeshBrushSettings settings = base_settings();
    settings.strength = 1.0f;
    settings.color = cf3(1, 0, 0);

    std::vector<cfloat3> current{cf3(0, 0, 1), cf3(0, 0, 1), cf3(0, 0, 1), cf3(0, 0, 1),
                                 cf3(0, 0, 1)};
    std::vector<cfloat3> out = current;
    mesh::kernel_paint(s, settings, current.data(), out.data());

    // A full-weight paint lands EXACTLY on the target: the blend is exact at
    // both ends, or a fully-weighted dab leaves a one-ULP seam along its rim.
    CHECK(out[0].x == 1.0f);
    CHECK(out[0].z == 0.0f);
    // Half weight blends half way.
    CHECK(out[1].x == doctest::Approx(0.5f));

    // Smear with no direction is not a smooth: it writes nothing.
    out = current;
    settings.direction = cf3(0, 0, 0);
    mesh::kernel_smear(s, nb, settings, current.data(), out.data());
    for (std::size_t i = 0; i < s.count; ++i) CHECK(out[i].z == 1.0f);
}
