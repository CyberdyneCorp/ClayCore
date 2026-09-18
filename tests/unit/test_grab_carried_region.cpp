// A GRAB CARRIES THE REGION IT CAPTURED (brush-engine spec, change
// capture-the-grab-region, issue #620).
//
// The rule: a grab gathers its region ONCE, at its first stamp, and every stamp
// writes `captured + weight * (p_k - p_0)`. The weight-1 centre therefore
// follows the cursor exactly and the gesture reaches the whole drag, where
// re-gathering around a point the surface had already left reached 41% of a 0.6
// drag and 18% of a 1.5 one.
//
// WHY THESE CASES ASSERT COUNTS AND NOT ONLY A REACH. On an adaptive surface the
// rule is five rules, four of them about what the REMESH does to the carried
// region, and four of the five are invisible in a reach that happens to land
// near the right number. Measured over twelve fixtures while the change was
// designed: the maintenance alone keeps MORE entries live at some stamps than
// the full rule does and still loses a quarter of the drag, because the
// weight-1 centre was collapsed and a split's mean weight cannot recreate it. So
// every case here asserts the counters — entries carried, inserted by a split,
// retired by a collapse, collapses refused, and the top weight still carried —
// with its own preconditions asserted beside them, so a maintenance that
// silently does nothing cannot pass by landing near the right number.
//
// The fixtures are the ones where an unmaintained carried region DIES: brush
// radius 0.15 against detail resolution 4, where the remesher retired all nine
// captured entries inside one stroke and the reach fell to 2.8-7.0%, below the
// 9.3-22.3% that re-gathering reaches there. Every one is run with the drag's
// sign flipped as well, so a number here is the rule's and not the fixture's.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using brush::Stamp;
using brush::StrokePreset;
using brush::StrokeSample;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;

namespace {

// The same six-grid unit sphere the measurements were taken on.
Mesh cube_sphere(int n, float radius) {
    Mesh m;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const cfloat3 p = cf3(c[0], c[1], c[2]);
                const cfloat3 unit = p / clength(p);
                m.positions.push_back(unit * radius);
                m.normals.push_back(unit);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

std::vector<StrokeSample> line(cfloat3 a, cfloat3 b, int n) {
    std::vector<StrokeSample> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        out[static_cast<std::size_t>(i)].position = a + (b - a) * t;
        out[static_cast<std::size_t>(i)].pressure = 1.0f;
    }
    return out;
}

// A quarter arc from `from` to `to` about the origin, through `n` samples.
std::vector<StrokeSample> arc(cfloat3 from, cfloat3 to, int n) {
    std::vector<StrokeSample> out(static_cast<std::size_t>(n));
    const float r = clength(from);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float a = t * 3.14159265f * 0.5f;
        const cfloat3 p = from * std::cos(a) + to * std::sin(a);
        out[static_cast<std::size_t>(i)].position = p * (r / clength(p));
        out[static_cast<std::size_t>(i)].pressure = 1.0f;
    }
    return out;
}

std::vector<Stamp> stroke_of(const std::vector<StrokeSample>& samples, float radius,
                             float spacing) {
    StrokePreset preset;
    preset.radius = radius;
    preset.spacing = spacing;
    preset.strength = 1.0f;
    return brush::resolve_stroke(samples, preset);
}

MeshBrushSettings grab_settings() {
    MeshBrushSettings b;
    b.strength = 1.0f;
    b.geodesic = mesh::default_geodesic(MeshBrush::Grab);
    return b;
}

// THE REACH, defined so it survives a sign flip and a curve.
//
// The fixture is the UNIT sphere, so a vertex the drag moved is one that has
// LEFT it — outward for a pull, inward for a push. Among those and only those,
// the furthest travel along the drag from the gesture's first sample.
//
// Returned in WORLD UNITS. Divided by the drag's NET DISPLACEMENT it is the
// share of the drag; on a curve that displacement is the CHORD and not the path
// the cursor travelled, because `kernel_grab` is handed `p_n - p_0` and a case
// demanding the arc's length would be demanding 111% of what the rule promises.
float travel_of(const DynamicSurface& surface, cfloat3 origin, cfloat3 direction) {
    const cfloat3 dir = direction / clength(direction);
    float best = 0.0f;
    surface.vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        if (std::fabs(clength(v.position) - 1.0f) <= 1e-4f) return;
        best = std::max(best, cdot(v.position - origin, dir));
    });
    return best;
}

float travel_of(const Mesh& after, const Mesh& before, cfloat3 origin, cfloat3 direction) {
    const cfloat3 dir = direction / clength(direction);
    float best = 0.0f;
    for (std::size_t i = 0; i < after.positions.size(); ++i) {
        if (clength(after.positions[i] - before.positions[i]) <= 1e-5f) continue;
        best = std::max(best, cdot(after.positions[i] - origin, dir));
    }
    return best;
}

// The longest edge among the edges with BOTH endpoints inside a ball — "is the
// surface refined HERE", which a whole-surface maximum cannot answer.
float longest_edge_near(const DynamicSurface& surface, cfloat3 centre, float radius,
                        std::size_t* vertices) {
    const float r2 = radius * radius;
    float best = 0.0f;
    if (vertices != nullptr) {
        *vertices = 0;
        surface.vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
            if (cdot2(v.position - centre) <= r2) ++*vertices;
        });
    }
    surface.edges().for_each_live([&](mesh::EdgeId e, const mesh::DynamicEdge&) {
        const mesh::HalfEdgeId h = surface.halfedge_of(e);
        if (cdot2(surface.position_of(surface.origin_of(h)) - centre) > r2) return;
        if (cdot2(surface.position_of(surface.target_of(h)) - centre) > r2) return;
        best = std::max(best, surface.edge_length(e));
    });
    return best;
}

float longest_edge(const DynamicSurface& surface) {
    float best = 0.0f;
    surface.edges().for_each_live([&](mesh::EdgeId e, const mesh::DynamicEdge&) {
        best = std::max(best, surface.edge_length(e));
    });
    return best;
}

// One fixture: a drag over the unit sphere, with the brush and the detail it is
// measured at.
struct Fixture {
    const char* name;
    std::vector<StrokeSample> samples;
    float radius;
    float spacing;
    float detail;
    int grid;
};

// A grab on a FIXED mesh. There is no remesher here, so the carried region is
// exactly what the first gather found.
struct FixedRun {
    Mesh before, after;
    float travel = 0.0f;
    std::size_t applied = 0;
};

FixedRun run_fixed(const Fixture& f) {
    FixedRun out;
    out.before = cube_sphere(f.grid, 1.0f);
    out.after = out.before;
    MeshSculptor sculptor(out.after);
    const std::vector<Stamp> stamps = stroke_of(f.samples, f.radius, f.spacing);
    MeshBrushSettings b = grab_settings();
    b.radius = f.radius;
    out.applied = brush::apply_to_mesh(sculptor, stamps, MeshBrush::Grab, b);
    out.travel = travel_of(out.after, out.before, stamps.front().position,
                           stamps.back().position - stamps.front().position);
    return out;
}

// A grab on an ADAPTIVE surface, with everything the maintenance is asserted on.
// `tip_*` are measured around the LAST stamp, because that is where the remesh
// centre following the stamp is visible and the surface-wide maximum is not.
struct AdaptiveRun {
    float travel = 0.0f;
    float top_weight = 0.0f;
    float max_edge = 0.0f;
    float tip_edge = 0.0f;
    std::size_t tip_vertices = 0;
    std::size_t applied = 0;
    std::size_t entries = 0, stamps = 0;
    std::size_t splits = 0, collapses = 0, relaxed = 0;
    DynamicSculptor::CarriedRegionCounters carry;
    bool valid = false;
};

AdaptiveRun run_adaptive(const Fixture& f, bool topology_enabled = true) {
    AdaptiveRun out;
    auto surface = DynamicSurface::from_mesh(cube_sphere(f.grid, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    DynamicTopologySettings topo;
    topo.enabled = topology_enabled;
    topo.detail_resolution = f.detail;
    const std::vector<Stamp> stamps = stroke_of(f.samples, f.radius, f.spacing);
    MeshBrushSettings b = grab_settings();
    b.radius = f.radius;
    mesh::DynamicStampResult summary;
    out.applied = brush::apply_to_dynamic(sculptor, stamps, MeshBrush::Grab, b, topo, nullptr,
                                          nullptr, {}, &summary);
    out.stamps = stamps.size();
    out.travel = travel_of(*surface, stamps.front().position,
                           stamps.back().position - stamps.front().position);
    out.max_edge = longest_edge(*surface);
    out.tip_edge = longest_edge_near(*surface, stamps.back().position, f.radius,
                                     &out.tip_vertices);
    out.splits = summary.remesh.split;
    out.collapses = summary.remesh.collapsed;
    out.relaxed = summary.remesh.relaxed;
    out.valid = mesh::validate_dynamic_surface(*surface).ok;
    // READ AFTER THE STROKE, while the sculptor still holds what the gesture
    // built: `apply_to_dynamic` closes the capture, and the counters outlive it
    // by design so a caller can read what the gesture did.
    out.carry = sculptor.carried_counters();
    out.top_weight = out.carry.top_weight;
    out.entries = out.carry.carried;
    return out;
}

// The two presets every case below is written against. `hard` is where an
// unmaintained carried region dies outright; `baseline` is the issue's own
// preset and the control.
Fixture hard(const char* name, cfloat3 from, cfloat3 to) {
    return Fixture{name, line(from, to, 16), 0.15f, 0.1f, 4.0f, 24};
}
Fixture baseline(const char* name, cfloat3 from, cfloat3 to) {
    return Fixture{name, line(from, to, 16), 0.3f, 0.1f, 8.0f, 24};
}

float drag_of(const Fixture& f) {
    const std::vector<Stamp> stamps = stroke_of(f.samples, f.radius, f.spacing);
    return clength(stamps.back().position - stamps.front().position);
}

}  // namespace

// -- the reach ----------------------------------------------------------------

TEST_CASE("grab: a carried region reaches the whole drag on both representations") {
    // Both poles and both signs, so the threshold cannot be met by one
    // fixture's luck: an unmaintained carried region reaches 100% on one pole of
    // this sphere and 5.6% on the other.
    const Fixture fixtures[] = {
        hard("mirror pole, pull-out 0.6", cf3(0, 0, -1.0f), cf3(0, 0, -1.6f)),
        hard("mirror pole, push-in 0.6 (sign flipped)", cf3(0, 0, -1.0f), cf3(0, 0, -0.4f)),
        hard("push-in 0.6", cf3(0, 0, 1.0f), cf3(0, 0, 0.4f)),
        hard("pull-out 0.6 (sign flipped)", cf3(0, 0, 1.0f), cf3(0, 0, 1.6f)),
        hard("long push-in 1.5", cf3(0, 0, 1.0f), cf3(0, 0, -0.5f)),
        hard("long pull-out 1.5 (sign flipped)", cf3(0, 0, 1.0f), cf3(0, 0, 2.5f)),
        baseline("baseline pull-out 0.6", cf3(0, 0, 1.0f), cf3(0, 0, 1.6f)),
        baseline("baseline pull-out 1.5", cf3(0, 0, 1.0f), cf3(0, 0, 2.5f)),
    };
    for (const Fixture& f : fixtures) {
        INFO(f.name);
        const float drag = drag_of(f);
        REQUIRE(drag > 0.5f);

        const FixedRun fixed = run_fixed(f);
        const AdaptiveRun adaptive = run_adaptive(f);
        // PRECONDITIONS: both paths applied stamps, and the adaptive one really
        // did remesh. A fixture that never split is not measuring the
        // maintenance at all.
        REQUIRE(fixed.applied > 0);
        REQUIRE(adaptive.applied > 0);
        REQUIRE(adaptive.splits > 0);
        REQUIRE(adaptive.carry.captured > 0);
        CHECK(adaptive.valid);

        // THE WHOLE DRAG, on both. Not `== 1.0`: the rows read 99.99-100.03%,
        // and the overshoot is real — a carried vertex keeps the tangential
        // slide the relaxation gave it, so it ends a fraction past the drag.
        CHECK(fixed.travel / drag == doctest::Approx(1.0f).epsilon(0.005));
        CHECK(adaptive.travel / drag == doctest::Approx(1.0f).epsilon(0.005));
        // AND THE TWO REPRESENTATIONS AGREE, which is the constraint #619 holds
        // and the one an unmaintained region breaks by up to 0.34 world units.
        CHECK(std::fabs(fixed.travel - adaptive.travel) < 1e-3f);
    }
}

TEST_CASE("grab: a curved drag carries the region to the chord, not the arc") {
    // A quarter arc: the cursor travels further than the region is asked to
    // move, because `kernel_grab` is handed `p_n - p_0`. A case written against
    // the PATH would demand 111% of what the rule promises.
    Fixture f{"curved +Z -> +X", arc(cf3(0, 0, 1.0f), cf3(1.0f, 0, 0), 16), 0.3f, 0.1f, 8.0f, 24};
    const std::vector<Stamp> stamps = stroke_of(f.samples, f.radius, f.spacing);
    float path = 0.0f;
    for (std::size_t i = 1; i < stamps.size(); ++i)
        path += clength(stamps[i].position - stamps[i - 1].position);
    const float chord = clength(stamps.back().position - stamps.front().position);
    REQUIRE(path > chord * 1.05f);  // the fixture really is a curve

    const FixedRun fixed = run_fixed(f);
    const AdaptiveRun adaptive = run_adaptive(f);
    REQUIRE(adaptive.splits > 0);
    // THE FIXED PATH IS THE CHORD EXACTLY: its carried region is the classes the
    // first gather found, and its weight-1 centre moves by `p_n - p_0`.
    CHECK(fixed.travel / chord == doctest::Approx(1.0f).epsilon(0.005));
    // And it is the CHORD and not the PATH: measured against the path it reads
    // about 90%, which is what a host comparing the surface with the cursor's
    // trail will see and file as a shortfall. It is not one.
    CHECK(fixed.travel < path * 0.98f);

    // THE ADAPTIVE SURFACE REACHES AT LEAST THE CHORD, and on a curve it reaches
    // PAST it — measured 116% against the fixed path's 100%. That is the
    // maintenance and not the deformation: the remesh splits inside the region
    // as the gesture sweeps the arc, and a child adopted from one carried parent
    // is born on material the arc has already carried and then takes its own
    // share of the remaining drag. The straight fixtures have no such material,
    // which is why they agree to 1e-4 and this one does not.
    CHECK(adaptive.travel >= chord * 0.995f);
    CHECK(adaptive.travel < chord * 1.3f);

    // ASSERTED, not asserted-about: with the remesher off the adaptive surface
    // has no material to pick up and no split to adopt, and the same deformation
    // rule lands on the fixed path's answer. So the overshoot above is the
    // maintenance's and the rule itself does not diverge between the two
    // representations.
    const AdaptiveRun frozen = run_adaptive(f, /*topology_enabled=*/false);
    REQUIRE(frozen.splits == 0);
    CHECK(std::fabs(fixed.travel - frozen.travel) < 1e-3f);
}

// -- the maintenance, as counts ------------------------------------------------

TEST_CASE("grab: the remesh maintains the carried region rather than losing it") {
    const Fixture fixtures[] = {
        hard("long push-in 1.5", cf3(0, 0, 1.0f), cf3(0, 0, -0.5f)),
        hard("long pull-out 1.5 (sign flipped)", cf3(0, 0, 1.0f), cf3(0, 0, 2.5f)),
        baseline("baseline pull-out 1.5", cf3(0, 0, 1.0f), cf3(0, 0, 2.5f)),
    };
    for (const Fixture& f : fixtures) {
        INFO(f.name);
        const AdaptiveRun r = run_adaptive(f);
        // PRECONDITIONS. A surface that never remeshed proves nothing about a
        // region maintained across a remesh, and neither does one whose region
        // was never captured.
        REQUIRE(r.carry.captured > 0);
        REQUIRE(r.splits > 0);
        REQUIRE(r.collapses > 0);

        // THE REGION IS MAINTAINED, ENTRY FOR ENTRY: what it carries at the end
        // is what it captured, plus what the splits inserted, minus what a
        // collapse retired.
        CHECK(r.entries == r.carry.captured + r.carry.inserted + r.carry.inserted_one_parent -
                               r.carry.retired);
        // It GREW rather than decaying — measured 9 entries becoming 195 and 45
        // becoming 792 on these two drags — which is the splits repopulating the
        // region the gesture stretched.
        CHECK(r.entries > r.carry.captured);
        // BOTH SPLIT RULES FIRED. A split with one carried parent is a large
        // minority of the splits inside a grab region — 42 of 74 on the first
        // stamp of one fixture — and dropping them costs the surface and makes
        // the protection refuse four times as many collapses.
        CHECK(r.carry.inserted > 0);
        CHECK(r.carry.inserted_one_parent > 0);

        // NOTHING WAS RETIRED, which is the sharpest assertion available here
        // and the one a broken protection fails first.
        CHECK(r.carry.retired == 0);
        // And the protection was actually asked: a fixture where the remesher
        // never wanted a carried vertex is not testing the refusal.
        CHECK(r.carry.collapses_refused > 0);
        // Collapses AVOIDED, not refusal events. The remesher re-asks about an
        // edge on each of its passes on each stamp, so an event count would run
        // an order of magnitude above the collapses it prevented and would be
        // bounded by nothing. Counted once per carried vertex for the gesture,
        // the REGION bounds it — which is the assertion an event count could not
        // be given.
        CHECK(r.carry.collapses_refused <= r.entries);

        // THE WEIGHT THAT CARRIES THE DRAG IS STILL THERE. This is the
        // assertion a count alone cannot make: maintenance without the collapse
        // protection keeps MORE entries live at some stamps and still loses a
        // quarter of the drag, because the weight-1 centre was collapsed and a
        // split's child takes the MEAN of its parents' weights.
        CHECK(r.top_weight == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(r.valid);
    }
}

TEST_CASE("grab: refusing those collapses leaves the surface finer, not coarser") {
    // The cost this rule was rejected on in advance, measured. Against a surface
    // with no remesher at all — which is what suppressing the remesh inside the
    // gesture would leave — the protected rule leaves a much finer surface, so
    // the refusal is not the adaptive representation giving up its work.
    const Fixture f = hard("long push-in 1.5", cf3(0, 0, 1.0f), cf3(0, 0, -0.5f));
    const AdaptiveRun remeshed = run_adaptive(f, /*topology_enabled=*/true);
    const AdaptiveRun frozen = run_adaptive(f, /*topology_enabled=*/false);
    REQUIRE(remeshed.splits > 0);
    REQUIRE(frozen.splits == 0);  // the control really did not remesh
    // Measured: 0.269 with the rule, 1.315 with no remesher, against a sphere
    // that starts uniform at 0.092.
    CHECK(remeshed.max_edge < frozen.max_edge * 0.5f);
    CHECK(remeshed.max_edge < 0.4f);
    // And the gesture still reaches the whole drag on both.
    const float drag = drag_of(f);
    CHECK(remeshed.travel / drag == doctest::Approx(1.0f).epsilon(0.005));
    CHECK(frozen.travel / drag == doctest::Approx(1.0f).epsilon(0.005));
}

TEST_CASE("grab: the remesh follows the stamp rather than the gesture's first sample") {
    // A remesh left at the first sample while the surface is dragged five brush
    // radii away refines nothing the gesture stretched: measured, it left a
    // longest edge of 1.12 against 0.36 with the centre following.
    const Fixture f = baseline("baseline pull-out 1.5", cf3(0, 0, 1.0f), cf3(0, 0, 2.5f));
    const AdaptiveRun r = run_adaptive(f);
    REQUIRE(r.splits > 0);
    const float drag = drag_of(f);
    REQUIRE(drag > 1.4f);

    // ASSERTED AT THE TIP, and that is the whole of what this case learned.
    // The SURFACE-WIDE longest edge does not gate this rule: it is in the NECK
    // behind the tip either way, and it reads 0.1466 with the centre following
    // and 0.1450 without — so a case written against it passes with the rule
    // removed. It was, and it did.
    //
    // At the tip the two are 3x apart, because a ball left at the gesture's
    // first sample never reaches five brush radii away: the tip's longest edge
    // is 0.0487 with the centre following and 0.1450 without, and there are 65
    // vertices within a brush radius of the tip against 25.
    //
    // The threshold is the REMESHER'S OWN: an edge above `target * split_factor`
    // is one it would have split, so "the tip is refined" is exactly "no edge
    // there is one the remesher still owes a split".
    DynamicTopologySettings topo;
    topo.detail_resolution = f.detail;
    CHECK(r.tip_edge <= topo.target_for(f.radius) * topo.split_factor);
    CHECK(r.tip_edge < r.max_edge);
    CHECK(r.tip_vertices > 40);
    MESSAGE("tip_edge=" << r.tip_edge << " tip_vertices=" << r.tip_vertices
                        << " max_edge=" << r.max_edge << " splits=" << r.splits
                        << " collapses=" << r.collapses << " entries=" << r.entries
                        << " inserted=" << r.carry.inserted << "/"
                        << r.carry.inserted_one_parent << " refused="
                        << r.carry.collapses_refused << " travel=" << r.travel);
}

TEST_CASE("grab: a carried vertex the remesher moved keeps that move") {
    // RULE 5, asserted directly rather than through a reach. A collapse places
    // its survivor at the midpoint and the relaxation slides vertices along the
    // surface; both move vertices the gesture is carrying. Their captured
    // positions take the same shift, or the next stamp writes
    // `captured + w * drag` and puts them back.
    //
    // The probe: after the drag, one more stamp at the SAME position with the
    // remesh switched off. Its write is `captured + w * total`, which is where
    // every carried vertex already is — so it changes nothing. Without the
    // shift it would drag every carried vertex back by everything the remesher
    // had moved it since its last write.
    const Fixture f = baseline("baseline pull-out 1.5", cf3(0, 0, 1.0f), cf3(0, 0, 2.5f));
    auto surface = DynamicSurface::from_mesh(cube_sphere(f.grid, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    DynamicTopologySettings topo;
    topo.detail_resolution = f.detail;
    REQUIRE(topo.relax_after_remesh);  // the slide this rule exists for
    const std::vector<Stamp> stamps = stroke_of(f.samples, f.radius, f.spacing);
    const cfloat3 first = stamps.front().position;

    MeshBrushSettings b = grab_settings();
    b.radius = f.radius;
    sculptor.begin_carried_region();
    std::size_t relaxed = 0;
    for (const Stamp& s : stamps) {
        MeshBrushSettings stamp_settings = b;
        stamp_settings.radius = s.radius;
        stamp_settings.direction = s.position - first;
        stamp_settings.center = sculptor.carrying() ? s.position : first;
        relaxed += sculptor.stamp(MeshBrush::Grab, stamp_settings, topo).remesh.relaxed;
    }
    // PRECONDITIONS: the remesher really did move carried vertices, and the
    // relaxation really did run.
    REQUIRE(relaxed > 0);
    REQUIRE(sculptor.carried_counters().moved > 0);
    REQUIRE(sculptor.carrying());

    // The same write, one more time, with nothing new to drag and no remesh.
    std::vector<cfloat3> before;
    surface->vertices().for_each_live(
        [&](mesh::VertexId, const mesh::DynamicVertex& v) { before.push_back(v.position); });
    DynamicTopologySettings off;
    off.enabled = false;
    MeshBrushSettings again = b;
    again.radius = stamps.back().radius;
    again.direction = stamps.back().position - first;
    again.center = stamps.back().position;
    const std::size_t moved = sculptor.stamp(MeshBrush::Grab, again, off).moved_vertices;
    REQUIRE(moved > 0);  // the write ran; the question is whether it CHANGED anything
    float worst = 0.0f;
    std::size_t i = 0;
    surface->vertices().for_each_live([&](mesh::VertexId, const mesh::DynamicVertex& v) {
        worst = std::max(worst, clength(v.position - before[i++]));
    });
    sculptor.end_carried_region();
    // Bit-equal but for the rounding of `captured + shift` against `after`.
    CHECK(worst < 1e-4f);
}

// -- the fixed and multiresolution paths --------------------------------------

TEST_CASE("grab: a fixed mesh is walked once for the gesture, not once per stamp") {
    // The saving the capture buys, asserted as the COUNT it is and not as a
    // duration: the gather's cost is the vertices it considers, and a gesture
    // that captured its region considers the same entries every stamp without
    // walking the surface for them.
    const Fixture f = baseline("baseline pull-out 0.6", cf3(0, 0, 1.0f), cf3(0, 0, 1.6f));
    const std::vector<Stamp> stamps = stroke_of(f.samples, f.radius, f.spacing);
    REQUIRE(stamps.size() > 4);

    Mesh m = cube_sphere(f.grid, 1.0f);
    MeshSculptor sculptor(m);
    mesh::SculptCounters counters;
    sculptor.set_counters(&counters);
    MeshBrushSettings b = grab_settings();
    b.radius = f.radius;
    REQUIRE(brush::apply_to_mesh(sculptor, stamps, MeshBrush::Grab, b) > 0);
    // Every stamp read the SAME captured region, so the entries considered are
    // exactly the capture repeated — which is what "one gather" means where a
    // re-gather would have found a different, shrinking region each time.
    REQUIRE(sculptor.carried_entries() == 0);  // the scope closed with the stroke
    REQUIRE(counters.vertices_considered > 0);
    CHECK(counters.vertices_considered % stamps.size() == 0);
}

TEST_CASE("grab: a multiresolution grab reaches the whole drag") {
    // THE FIRST GRAB THROUGH `apply_to_multires` IN THE TREE. It shares
    // `mesh_stamp_settings` and drives a `MeshSculptor` per level, so it takes
    // the rule by construction — which is worth a case rather than an argument.
    Mesh base = cube_sphere(16, 1.0f);
    auto surface = mesh::MultiresSurface::from_mesh(base);
    REQUIRE(surface.has_value());
    mesh::MultiresSculptor sculptor(*surface);
    const std::vector<Stamp> stamps =
        stroke_of(line(cf3(0, 0, 1.0f), cf3(0, 0, 1.6f), 16), 0.3f, 0.1f);
    MeshBrushSettings b = grab_settings();
    b.radius = 0.3f;
    REQUIRE(brush::apply_to_multires(sculptor, stamps, MeshBrush::Grab, b) > 0);

    const Mesh& after = surface->level_mesh(surface->sculpt_level());
    const float drag = clength(stamps.back().position - stamps.front().position);
    float travel = 0.0f;
    for (const cfloat3& p : after.positions) {
        if (std::fabs(clength(p) - 1.0f) <= 1e-4f) continue;
        travel = std::max(travel, cdot(p - stamps.front().position,
                                       (stamps.back().position - stamps.front().position) / drag));
    }
    CHECK(travel / drag == doctest::Approx(1.0f).epsilon(0.02));
}
