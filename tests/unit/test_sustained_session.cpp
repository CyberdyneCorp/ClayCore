// DOES A WARM DAB COST THE SAME AFTER TEN THOUSAND OF THEM
// (gate-sustained-device-sculpting).
//
// The device gate measures growth over DOCUMENT SIZE — `GrowthAxis.standard` is
// [10, 100, 1000] items, and a case fails when its cost scales faster than
// N^1.25. Nothing measured growth over SESSION LENGTH, and the two are different
// failure modes:
//
//   over document size   catches an algorithm that is not local
//   over session length  catches a leak, an unbounded cache, a history
//                        outgrowing its budget, an arena that never converges
//
// An artist's session is hours of dabs on ONE model. The first axis says nothing
// about it.
//
// -- WHY THE FIXTURE IS A GRAB AND NOT A DRAW ---------------------------------
//
// A sculpt fixture cannot hold its own workset constant while actually
// deforming, because the falloff is measured ALONG THE SURFACE IT IS CHANGING. A
// draw looped on one circle builds a bump, the bump lengthens the geodesic
// distance to the same world radius, and a later dab genuinely reaches fewer
// vertices — MEASURED AT 2.1x FEWER over 27 revolutions. A gate reading that
// would be reading the geometry it was changing and calling it a session.
//
// Alternating the draw's sign does not fix it either: a draw deposits along the
// region's AVERAGED NORMAL, which the deformation itself turns, so +s and -s do
// not cancel. Over 82 revolutions the workset still fell by a third.
//
// A GRAB displaces by `direction * weight` and names no normal, so alternating
// the direction per revolution returns the surface to where it started. Measured
// over 29,520 dabs, the late window reaches 1,965,254 vertices against the early
// window's 1,966,791 — 0.08% apart, and that residue is float addition not
// cancelling exactly rather than anything drifting.
//
// -- WHAT IS ASSERTED ---------------------------------------------------------
//
// THE COUNTS, which are the same integers on every machine, and the arena's
// convergence. NOT the wall clock: this file runs in CI on a shared runner, and
// a duration there is a claim about the scheduler. The recorded timings are in
// the change's proposal as evidence, and the DEVICE harness is where a latency
// band belongs.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/mesh/mesh_data.h"
#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::mesh;
using kernel::cf3;

namespace {

Mesh plane(int n, float spacing) {
    Mesh m;
    const int centre = n / 2;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(spacing * static_cast<float>(x - centre),
                                      ((x + z) & 1) ? spacing * 0.5f : 0.0f,
                                      spacing * static_cast<float>(z - centre)));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// One revolution of the circle, and the sign flip that returns the surface.
constexpr int kPerRevolution = 120;
// An EVEN number of revolutions per window: the sign alternates per revolution,
// so an odd count would leave each window ending on a different phase of the
// surface and the counts would differ by the phase rather than by the session.
constexpr int kRevolutionsPerWindow = 6;
constexpr int kPerWindow = kPerRevolution * kRevolutionsPerWindow;

struct Window {
    std::uint64_t considered = 0;
    std::uint64_t affected = 0;
    std::uint64_t faces = 0;
    std::uint64_t scratch_at_end = 0;
    std::uint64_t history_at_end = 0;
};

}  // namespace

TEST_CASE("sustained: a warm dab does not drift over a long session") {
    Mesh m = plane(96, 0.02f);
    MeshSculptor s(m);
    SculptCounters counters;
    s.set_counters(&counters);

    MeshBrushSettings brush;
    brush.radius = 0.10f;
    brush.strength = 1.0f;
    brush.geodesic = default_geodesic(MeshBrush::Grab);

    VertexDeltas record;
    Window windows[3];
    std::uint64_t last_considered = 0, last_affected = 0, last_faces = 0;

    for (int i = 0; i < kPerWindow * 3; ++i) {
        const float t = 2.0f * 3.14159265358979f *
                        static_cast<float>(i % kPerRevolution) /
                        static_cast<float>(kPerRevolution);
        brush.center = cf3(0.25f * std::cos(t), 0.0f, 0.25f * std::sin(t));
        const float sign = ((i / kPerRevolution) % 2 == 0) ? 1.0f : -1.0f;
        brush.direction = cf3(0.0f, sign * 0.0008f, 0.0f);

        s.stamp(MeshBrush::Grab, brush, {}, &record);

        Window& w = windows[i / kPerWindow];
        w.considered += counters.vertices_considered - last_considered;
        w.affected += counters.vertices_affected - last_affected;
        w.faces += counters.faces_touched - last_faces;
        last_considered = counters.vertices_considered;
        last_affected = counters.vertices_affected;
        last_faces = counters.faces_touched;
        w.scratch_at_end = counters.scratch_high_water;
        w.history_at_end = record.bytes();
    }

    REQUIRE(windows[0].considered > 0);
    REQUIRE(windows[0].affected > 0);
    REQUIRE(windows[0].faces > 0);

    // THE SAME DABS, DONE LATER, TOUCH THE SAME SURFACE. A tolerance rather than
    // equality, and the tolerance is named: the grab's cancellation is float
    // addition, which does not return the vertex to the same bits, so the
    // surface drifts microscopically and the walk answers microscopically
    // differently. Measured at 0.08% over 29,520 dabs; 2% here is wide enough
    // that the residue cannot fail it and narrow enough that the 2.1x a draw
    // fixture produces could not pass.
    const auto within = [](std::uint64_t early, std::uint64_t late, double tol) {
        const double a = static_cast<double>(early), b = static_cast<double>(late);
        return std::fabs(b - a) <= tol * a;
    };
    INFO("considered early " << windows[0].considered << " late " << windows[2].considered);
    CHECK(within(windows[0].considered, windows[2].considered, 0.02));
    INFO("affected early " << windows[0].affected << " late " << windows[2].affected);
    CHECK(within(windows[0].affected, windows[2].affected, 0.02));
    INFO("faces early " << windows[0].faces << " late " << windows[2].faces);
    CHECK(within(windows[0].faces, windows[2].faces, 0.02));

    // THE ARENA CONVERGED. A per-stamp scratch that is still growing in the last
    // window is scratch that is never released, and no allocation count can see
    // it — the high-water mark is the only thing that can.
    CHECK(windows[1].scratch_at_end == windows[2].scratch_at_end);

    // THE HISTORY IS BOUNDED BY WHAT THE STROKE REACHED, not by how many stamps
    // it took. `VertexDeltas` coalesces per gesture — a vertex touched by a
    // thousand stamps appears once — and this is what proves it: the record
    // stops growing while the stamps keep coming.
    CHECK(windows[1].history_at_end == windows[2].history_at_end);
    CHECK(windows[2].history_at_end > 0);
}

TEST_CASE("sustained: a draw fixture would be reading the geometry, not the session") {
    // THE GATE ABOVE IS ONLY MEANINGFUL IF ITS FIXTURE COULD FAIL IT. This is
    // the fixture that was tried first: the same loop with a Draw, whose deposit
    // follows the region's averaged normal. It builds a bump, the bump lengthens
    // the geodesic distance to the same world radius, and the workset falls away
    // under it.
    //
    // Asserted as a REAL DROP rather than as a tolerance, so that if some future
    // change made a draw fixture stable this case fails and says the gate above
    // may be over-specified.
    Mesh m = plane(96, 0.02f);
    MeshSculptor s(m);
    SculptCounters counters;
    s.set_counters(&counters);

    MeshBrushSettings brush;
    brush.radius = 0.10f;
    brush.strength = 0.02f;
    brush.direction = cf3(0, 1, 0);
    brush.geodesic = default_geodesic(MeshBrush::Draw);

    std::uint64_t first = 0, last = 0, previous = 0;
    for (int i = 0; i < kPerWindow * 3; ++i) {
        const float t = 2.0f * 3.14159265358979f *
                        static_cast<float>(i % kPerRevolution) /
                        static_cast<float>(kPerRevolution);
        brush.center = cf3(0.25f * std::cos(t), 0.0f, 0.25f * std::sin(t));
        s.stamp(MeshBrush::Draw, brush);
        const std::uint64_t reached = counters.vertices_considered - previous;
        previous = counters.vertices_considered;
        if (i == 0) first = reached;
        last = reached;
    }
    INFO("a draw fixture reached " << first << " then " << last);
    REQUIRE(first > 0);
    CHECK(last < first);
}
